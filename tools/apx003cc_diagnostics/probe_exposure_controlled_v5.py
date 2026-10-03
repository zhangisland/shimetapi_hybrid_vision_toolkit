#!/usr/bin/env python3
"""Controlled candidate A/B/A experiment; a brightness response is NOT exposure proof."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import sys
import threading
import time
from collections import deque

ROOT = Path(__file__).resolve().parents[2]

class Bus:
    def __init__(self, bus, address):
        self.bus, self.address = bus, address
    def transfer(self, parts):
        # No force: respect a bound kernel driver's ownership.
        p = subprocess.run(['i2ctransfer', '-y', str(self.bus), *parts], capture_output=True, text=True, timeout=3)
        if p.returncode:
            raise RuntimeError('I2C transfer failed: '+p.stderr.strip())
        return p.stdout
    def read(self, reg):
        out = self.transfer([f'w2@{self.address:#x}', f'{reg>>8:#x}', f'{reg&255:#x}', 'r1'])
        return int(out.strip(), 16)
    def write(self, reg, value):
        self.transfer([f'w3@{self.address:#x}', f'{reg>>8:#x}', f'{reg&255:#x}', f'{value:#x}'])
    def latch(self):
        for value in (0, 1, 0): self.write(0x342c, value)
    def gain(self, db):
        for reg, value in [(0x3603,7),(0x3660,1),(0x3602,255 if db==0 else 16),(0x3661,1),(0x3662,0)]:
            self.write(reg,value)
        self.latch()
        if self.read(0x3602)!=(255 if db==0 else 16): raise RuntimeError('Gain readback mismatch')
    def snapshot(self):
        return {f'{r:#06x}':self.read(r) for r in [*range(0x3251,0x3262),0x3125,0x3502,0x340c,0x3602,0x3603]}

class Preview:
    def __init__(self, output):
        self.rows=deque(maxlen=128)
        self.lock=threading.Lock()
        self.log=open(output/'preview.log','w',buffering=1)
        # No --aps-gain-db: this process must never compete with probe I2C writes.
        self.proc=subprocess.Popen([sys.executable,'-u','hvs.py','live','--x5-vin-bypass','--','--preview-width','640','--no-verify'],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
        self.thread=threading.Thread(target=self.pump,daemon=True);self.thread.start()
    def pump(self):
        for line in self.proc.stdout:
            self.log.write(line)
            if not line.startswith('Preview APS received='): continue
            values={k:float(v) for k,v in re.findall(r'(received|packets|displayed|raw_mean|raw_sat_percent)=([0-9.]+)',line)}
            if len(values)==5:
                values['time']=time.monotonic()
                with self.lock:self.rows.append(values)
    def measure(self, settle=1.5, window=3):
        start=time.monotonic()+settle;end=start+window
        while time.monotonic()<end:
            if self.proc.poll() is not None: raise RuntimeError('Preview exited; see preview.log')
            time.sleep(.05)
        with self.lock:rows=[dict(r) for r in self.rows if start<=r['time']<=end]
        return evaluate(rows)
    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.terminate();self.proc.wait(timeout=8)
        self.thread.join(timeout=3);self.log.close()

def evaluate(rows):
    if len(rows)<2: raise RuntimeError('Insufficient fresh telemetry samples')
    for a,b in zip(rows,rows[1:]):
        dt=b['time']-a['time']
        if not 0<dt<2.5 or any(b[k]<=a[k] for k in ('received','packets','displayed')):
            raise RuntimeError('STALE / stopped APS, EVS or displayed RAW sample; window INVALID')
        if min(b['received']-a['received'],b['packets']-a['packets'])/dt<20:
            raise RuntimeError('Stream rate below 20/s; candidate changes/stalls timing, window INVALID')
    means=[r['raw_mean'] for r in rows]
    return {'raw_mean':statistics.mean(means),'raw_std':statistics.pstdev(means),
            'raw_sat_percent':statistics.mean(r['raw_sat_percent'] for r in rows),
            'aps_first':rows[0]['received'],'aps_last':rows[-1]['received'],
            'evs_first':rows[0]['packets'],'evs_last':rows[-1]['packets'],
            'displayed_first':rows[0]['displayed'],'displayed_last':rows[-1]['displayed'],
            'aps_rate':(rows[-1]['received']-rows[0]['received'])/(rows[-1]['time']-rows[0]['time']),
            'evs_packet_rate':(rows[-1]['packets']-rows[0]['packets'])/(rows[-1]['time']-rows[0]['time']),
            'samples':len(rows),'window_s':rows[-1]['time']-rows[0]['time']}

def stable(reference, current):
    # Black-corrected signal drift; black=64 is a diagnostic estimate, not calibration.
    allowance=max(3.,abs(reference['raw_mean']-64)*.12,6*reference['raw_std'])
    if abs(current['raw_mean']-reference['raw_mean'])>allowance:
        raise RuntimeError('Baseline did not recover: possible latch / scene change; STOP, no further writes')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--register',type=lambda x:int(x,0),choices=[0x3253,0x3252,0x3125],required=True)
    p.add_argument('--values',nargs='+',type=lambda x:int(x,0),required=True)
    p.add_argument('--i2c-bus',type=int,default=6)
    p.add_argument('--i2c-address',type=lambda x:int(x,0),default=0x3c)
    p.add_argument('--gain-db',choices=['auto','0','24'],default='auto')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    if any(v<0 or v>255 for v in args.values):p.error('values must be bytes')
    if not os.environ.get('DISPLAY'):p.error('Run in a desktop/X11 session')
    args.output.mkdir(parents=True,exist_ok=False)
    bus=Bus(args.i2c_bus,args.i2c_address);preview=None;original=None;result={'status':'running','register':hex(args.register),'trials':[]}
    def emit(label,data):
        print(label+' '+json.dumps(data),flush=True)
        with (args.output/'measurements.jsonl').open('a') as f:f.write(json.dumps({'label':label,**data})+'\n')
    try:
        preview=Preview(args.output)
        preview.measure(settle=5) # Camera powered and both streams alive before I2C.
        factory=bus.snapshot();original=factory[f'{args.register:#06x}'];result['factory']=factory
        emit('factory',factory)
        if factory['0x3502']!=3 or factory['0x340c']!=1:raise RuntimeError('Unexpected fresh-init mode; refuse contaminated baseline')
        bus.gain(0);zero=preview.measure();emit('gain0',zero)
        bus.gain(24);high=preview.measure();emit('gain24_positive_control',high)
        if high['raw_mean']<zero['raw_mean']*1.3 or high['raw_sat_percent']>10:
            raise RuntimeError('Optical sensitivity anchor invalid: insufficient gain response or saturation; adjust scene')
        bus.gain(0);restored=preview.measure();stable(zero,restored);emit('gain0_restored',restored)
        gain=(0 if zero['raw_mean']-64>=20 else 24) if args.gain_db=='auto' else int(args.gain_db)
        bus.gain(gain);baseline=preview.measure();result['gain_db']=gain;result['baseline']=baseline;emit('baseline',baseline)
        if baseline['raw_mean']-64<20 or baseline['raw_sat_percent']>10:raise RuntimeError('Baseline insufficient signal or clipped; no candidate experiment')
        for value in args.values:
            stable(baseline,preview.measure(settle=.5))
            guard=bus.snapshot()
            if guard['0x3502']!=factory['0x3502'] or guard['0x3602']!=(255 if gain==0 else 16):raise RuntimeError('Mode/gain guard changed')
            bus.write(args.register,value);bus.latch();immediate=bus.read(args.register)
            emit('write_audit',{'requested':value,'readback':immediate,'register':hex(args.register)})
            measured=preview.measure();after=bus.snapshot()
            trial={'requested':value,'readback':immediate,'after':after,'measurement':measured}
            result['trials'].append(trial);emit('candidate',trial)
            if any(abs(measured[k]/baseline[k]-1)>.10 for k in ('aps_rate','evs_packet_rate')):
                raise RuntimeError('Candidate changes dual-stream rate >10%; not an isolated exposure control; restoring and STOP')
            if after[f'{args.register:#06x}']!=immediate:raise RuntimeError('Candidate readback not held')
            if after['0x3502']!=factory['0x3502'] or after['0x3602']!=guard['0x3602']:raise RuntimeError('Mode/gain changed; cannot isolate candidate')
            bus.write(args.register,original);bus.latch()
            if bus.read(args.register)!=original:raise RuntimeError('Restore register failed')
            recovery=preview.measure();trial['recovery']=recovery;emit('restored',recovery);stable(baseline,recovery)
        result['status']='complete';result['verdict']='Optical response only; integration time and units remain unverified'
    except (Exception,KeyboardInterrupt) as error:
        result['status']='invalid_or_aborted';result['error']=str(error);emit('ABORT',{'reason':str(error)})
    finally:
        if preview:
            if original is not None and preview.proc.poll() is None:
                try:bus.write(args.register,original);bus.latch();bus.gain(0)
                except Exception as e:result['cleanup_error']=str(e)
            preview.stop()
        (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return 0 if result['status']=='complete' else 2

if __name__=='__main__':sys.exit(main())
