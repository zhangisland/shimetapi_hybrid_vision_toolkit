/* Pipeline setup adapted from D-Robotics dual_vc_vin.c (2024 copyright).
 * Local reference: sample_apx003cc/sample_hvs/dual_vc_vin_src.
 * No Camera ABI patching. Sensor configuration objects are supplied by the
 * matching official vp_sensors sources; use only one capture instance/process.
 */
#include "x5_capture.h"
#include "hb_camera_interface.h"
#include "hbn_api.h"
#include "hbn_isp_api.h"
#include "vp_sensors.h"
#include "hvs_config.h"
#include "x5_exposure_control.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t capture_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
#define VC_PIPE_NUM 2
#define APS_ISP_VC_INDEX 1U
#define ERR_CON_EQ(ret, expected) do { if ((ret)!=(expected)) { fprintf(stderr,"%s:%d SDK error %d\n",__func__,__LINE__,(int)(ret)); return ret; } } while(0)
typedef struct {
    hbn_vflow_handle_t vflow_fd;
    hbn_vnode_handle_t vin_node_handle, isp_node_handle;
    camera_handle_t cam_fd;
    vp_sensor_config_t *sensor_config;
    vp_csi_config_t csi_config;
} pipe_contex_t;
typedef struct { pipe_contex_t pipe_contex; int sensor_index; uint32_t link_port; } dual_vc_pipe_t;
struct x5_capture { dual_vc_pipe_t pipes[2]; int memory_open; x5_exposure_state exposure;
    hbn_vnode_image_t raw_slots[2]; /* one outstanding lease per VC in raw build */
};
static int settle = -1;
static int find_sensor_index_by_config_file(const char *config_file)
{
	int total = vp_get_sensors_list_number();

	for (int i = 0; i < total; i++) {
		if (vp_sensor_config_list[i] && vp_sensor_config_list[i]->config_file &&
			strcmp(vp_sensor_config_list[i]->config_file, config_file) == 0)
			return i;
	}
	return -1;
}

static int sensor_index_valid(int index)
{
	return index >= 0 && index < (int)vp_get_sensors_list_number();
}

static int configure_sensor(dual_vc_pipe_t *pipe, int sensor_index)
{
	vp_sensor_config_t *sensor_config = NULL;

	if (!sensor_index_valid(sensor_index)) {
		printf("Unsupported sensor index:%d\n", sensor_index);
		return -1;
	}

	sensor_config = vp_sensor_config_list[sensor_index];
	pipe->sensor_index = sensor_index;
	pipe->pipe_contex.sensor_config = sensor_config;

	printf("Using index:%d sensor_name:%s config_file:%s vc:%d\n",
		sensor_index, sensor_config->sensor_name, sensor_config->config_file,
		sensor_config->vin_node_attr->cim_attr.vc_index);

	if (sensor_config->sensor_type != SENSOR_TYPE_NORMAL) {
		printf("This sample only handles normal one-sensor multi-VC configs.\n");
		return -1;
	}

	if (settle >= 0 && settle <= 127) {
		sensor_config->camera_config->mipi_cfg->rx_attr.settle = settle;
	}

	return 0;
}

static int bind_same_mipi_host(dual_vc_pipe_t pipes[VC_PIPE_NUM])
{
	int32_t ret = 0;
	uint32_t mipi_rx = 0;

	ret = vp_sensor_fixed_mipi_host(pipes[0].pipe_contex.sensor_config,
		&pipes[0].pipe_contex.csi_config);
	if (ret != 0) {
		printf("No Camera Sensor found for sensor index:%d\n", pipes[0].sensor_index);
		return ret;
	}

	mipi_rx = pipes[0].pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
	pipes[1].pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx = mipi_rx;
	pipes[1].pipe_contex.csi_config = pipes[0].pipe_contex.csi_config;

	printf("Bind VC0 and VC1 VIN nodes to the same mipi_rx:%u\n", mipi_rx);
	return 0;
}

static int check_dual_vc_config(dual_vc_pipe_t pipes[VC_PIPE_NUM])
{
	uint32_t vc0 = pipes[0].pipe_contex.sensor_config->vin_node_attr->cim_attr.vc_index;
	uint32_t vc1 = pipes[1].pipe_contex.sensor_config->vin_node_attr->cim_attr.vc_index;
	uint32_t rx0 = pipes[0].pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;
	uint32_t rx1 = pipes[1].pipe_contex.sensor_config->vin_node_attr->cim_attr.mipi_rx;

	if (vc0 != 0 || vc1 != 1) {
		printf("Expected VC0 config first and VC1 config second, got vc:%u/%u\n",
			vc0, vc1);
		return -1;
	}

	if (rx0 != rx1) {
		printf("Expected both VC configs to use the same mipi_rx, got %u/%u\n",
			rx0, rx1);
		return -1;
	}

	return 0;
}

static int create_camera_node(dual_vc_pipe_t *pipe)
{
	int32_t ret = 0;

	ret = hbn_camera_create(pipe->pipe_contex.sensor_config->camera_config,
		&pipe->pipe_contex.cam_fd);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_vin_node(dual_vc_pipe_t *pipe)
{
	vp_sensor_config_t *sensor_config = pipe->pipe_contex.sensor_config;
	vin_node_attr_t *vin_node_attr = sensor_config->vin_node_attr;
	vin_ichn_attr_t *vin_ichn_attr = sensor_config->vin_ichn_attr;
	vin_ochn_attr_t *vin_ochn_attr = sensor_config->vin_ochn_attr;
	vin_attr_ex_t vin_attr_ex = {0};
	hbn_buf_alloc_attr_t alloc_attr = {0};
	hbn_vnode_handle_t *vin_node_handle = &pipe->pipe_contex.vin_node_handle;
	uint32_t hw_id = vin_node_attr->cim_attr.mipi_rx;
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	uint64_t vin_attr_ex_mask = 0;
	int32_t ret = 0;

	pipe->link_port = vin_node_attr->cim_attr.vc_index;

	if (pipe->pipe_contex.csi_config.mclk_is_not_configed) {
		printf("csi%d ignore mclk ex attr, because not config mclk.\n",
			pipe->pipe_contex.csi_config.index);
	} else {
		vin_attr_ex.vin_attr_ex_mask = sensor_config->vin_attr_ex->vin_attr_ex_mask;
		vin_attr_ex.mclk_ex_attr.mclk_freq =
			sensor_config->vin_attr_ex->mclk_ex_attr.mclk_freq;
		vin_attr_ex_mask = vin_attr_ex.vin_attr_ex_mask;
	}

	printf("Open VIN node: sensor:%s hw_id/mipi_rx:%u vc_index:%u\n",
		sensor_config->sensor_name, hw_id, pipe->link_port);

	ret = hbn_vnode_open(HB_VIN, hw_id, AUTO_ALLOC_ID, vin_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*vin_node_handle, vin_node_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*vin_node_handle, ichn_id, vin_ichn_attr);
	ERR_CON_EQ(ret, 0);

	vin_ochn_attr->ddr_en = 1;
	ret = hbn_vnode_set_ochn_attr(*vin_node_handle, ochn_id, vin_ochn_attr);
	ERR_CON_EQ(ret, 0);

	if (vin_attr_ex_mask) {
		for (uint8_t i = 0; i < VIN_ATTR_EX_INVALID; i++) {
			if ((vin_attr_ex_mask & (1 << i)) == 0)
				continue;

			vin_attr_ex.ex_attr_type = i;
			ret = hbn_vnode_set_attr_ex(*vin_node_handle, &vin_attr_ex);
			ERR_CON_EQ(ret, 0);
		}
	}

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
		| HB_MEM_USAGE_CPU_WRITE_OFTEN
		| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*vin_node_handle, ochn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	return 0;
}

static int create_isp_node(dual_vc_pipe_t *pipe)
{
	vp_sensor_config_t *sensor_config = pipe->pipe_contex.sensor_config;
	isp_attr_t *isp_attr = sensor_config->isp_attr;
	isp_ichn_attr_t *isp_ichn_attr = sensor_config->isp_ichn_attr;
	isp_ochn_attr_t *isp_ochn_attr = sensor_config->isp_ochn_attr;
	hbn_vnode_handle_t *isp_node_handle = &pipe->pipe_contex.isp_node_handle;
	hbn_buf_alloc_attr_t alloc_attr = {0};
	uint32_t ichn_id = 0;
	uint32_t ochn_id = 0;
	int32_t ret = 0;

	/* ISP offline mode: VIN writes RAW to DDR, vflow binding feeds ISP. */
	isp_attr->input_mode = 2;  // 0: online, 1: mcm, 2: offline

	ret = hbn_vnode_open(HB_ISP, 0, AUTO_ALLOC_ID, isp_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_attr(*isp_node_handle, isp_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ochn_attr(*isp_node_handle, ochn_id, isp_ochn_attr);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vnode_set_ichn_attr(*isp_node_handle, ichn_id, isp_ichn_attr);
	ERR_CON_EQ(ret, 0);

	alloc_attr.buffers_num = 3;
	alloc_attr.is_contig = 1;
	alloc_attr.flags = HB_MEM_USAGE_CPU_READ_OFTEN
		| HB_MEM_USAGE_CPU_WRITE_OFTEN
		| HB_MEM_USAGE_CACHED;
	ret = hbn_vnode_set_ochn_buf_attr(*isp_node_handle, ochn_id, &alloc_attr);
	ERR_CON_EQ(ret, 0);

	printf("Created ISP node for APS VC%u\n", pipe->link_port);
	return 0;
}

/* HVS sensor_mode=2 makes the AE engine run the hdrv31 path, which never
 * converges on this single-channel pipeline. Lock AE to manual exposure
 * (and AWB to manual gains) so the ISP outputs stable frames. */
static int isp_set_manual_2a(hbn_vnode_handle_t isp_node_handle, x5_exposure_state *state)
{
    hbn_isp_awb_attr_t awb_attr = {0};
    int ret = x5_apply_exposure(isp_node_handle, state);
    if (ret) return ret;

	ret = hbn_isp_get_awb_attr(isp_node_handle, &awb_attr);
	if (ret != 0) {
		printf("hbn_isp_get_awb_attr failed, ret:%d\n", ret);
		return ret;
	}
	awb_attr.mode = HBN_ISP_MODE_MANUAL;
	/* Neutral ISP AWB: residual WB belongs to the existing application algorithm. */
	awb_attr.manual_attr.gain.rgain = 1.0f;
	awb_attr.manual_attr.gain.grgain = 1.0f;
	awb_attr.manual_attr.gain.gbgain = 1.0f;
	awb_attr.manual_attr.gain.bgain = 1.0f;
	ret = hbn_isp_set_awb_attr(isp_node_handle, &awb_attr);
	if (ret != 0) {
		printf("hbn_isp_set_awb_attr failed, ret:%d\n", ret);
		return ret;
	}
	printf("ISP AWB locked to manual: rgain=%.3f bgain=%.3f\n",
		awb_attr.manual_attr.gain.rgain, awb_attr.manual_attr.gain.bgain);

	return 0;
}

static int create_and_run_vflow(dual_vc_pipe_t *pipe)
{
	/* Read vc_index from the sensor config: pipe->link_port is only set
	 * inside create_vin_node(), so it is still 0 here on first entry. */
	int with_isp = (pipe->pipe_contex.sensor_config->vin_node_attr->cim_attr.vc_index
		== APS_ISP_VC_INDEX);
#ifdef X5_RAW_ONLY
    with_isp = 0;
    pipe->pipe_contex.sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;
#endif
	int32_t ret = 0;

	/* Offline ISP requires VIN to write RAW to DDR instead of flyby. */
	if (with_isp)
		pipe->pipe_contex.sensor_config->vin_node_attr->cim_attr.cim_isp_flyby = 0;

	ret = create_camera_node(pipe);
	ERR_CON_EQ(ret, 0);

	ret = create_vin_node(pipe);
	ERR_CON_EQ(ret, 0);

	if (with_isp) {
		ret = create_isp_node(pipe);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_vflow_create(&pipe->pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_add_vnode(pipe->pipe_contex.vflow_fd,
		pipe->pipe_contex.vin_node_handle);
	ERR_CON_EQ(ret, 0);

	if (with_isp) {
		ret = hbn_vflow_add_vnode(pipe->pipe_contex.vflow_fd,
			pipe->pipe_contex.isp_node_handle);
		ERR_CON_EQ(ret, 0);

		ret = hbn_vflow_bind_vnode(pipe->pipe_contex.vflow_fd,
			pipe->pipe_contex.vin_node_handle, 0,
			pipe->pipe_contex.isp_node_handle, 0);
		ERR_CON_EQ(ret, 0);
	}

	ret = hbn_camera_attach_to_vin(pipe->pipe_contex.cam_fd,
		pipe->pipe_contex.vin_node_handle);
	ERR_CON_EQ(ret, 0);

	ret = hbn_vflow_start(pipe->pipe_contex.vflow_fd);
	ERR_CON_EQ(ret, 0);


	return 0;
}

static void cleanup_check(int ret, const char *op) { if(ret) fprintf(stderr,"%s failed: %d\n",op,ret); }
/* X5 SDK vnode_close and vflow_destroy return void; no status is available.
 * Keep return-code checks for vflow_stop and camera_destroy. */
static void stop_and_destroy_pipe(dual_vc_pipe_t *pipe)
{
	if (pipe->pipe_contex.vflow_fd)
		cleanup_check(hbn_vflow_stop(pipe->pipe_contex.vflow_fd), "pipeline cleanup");
	if (pipe->pipe_contex.vin_node_handle)
		hbn_vnode_close(pipe->pipe_contex.vin_node_handle);
	if (pipe->pipe_contex.isp_node_handle)
		hbn_vnode_close(pipe->pipe_contex.isp_node_handle);
	if (pipe->pipe_contex.cam_fd)
		cleanup_check(hbn_camera_destroy(pipe->pipe_contex.cam_fd), "pipeline cleanup");
	if (pipe->pipe_contex.vflow_fd)
		hbn_vflow_destroy(pipe->pipe_contex.vflow_fd);
}


void x5_close(x5_capture *capture) {
    if (!capture) return;
    for (int i=1; i>=0; --i) stop_and_destroy_pipe(&capture->pipes[i]);
    if (capture->memory_open) hb_mem_module_close();
    free(capture);
}
int x5_open(x5_capture **out, int profile, double exposure_us, double again, double dgain) {
    *out = NULL;
    x5_exposure_state state;
#ifdef X5_RAW_ONLY
    memset(&state,0,sizeof(state));
#else
    int validation=x5_exposure_request(&state,exposure_us,again,dgain);
    if(validation) return validation;
#endif
    const hvs_config_pair_t *cfg=hvs_config_get(profile);
    if (!cfg) return -1;
    x5_capture *c=calloc(1,sizeof(*c));
    if (!c) return -1;
    c->exposure=state;
    int ret=-1;
    for(int i=0;i<2;++i) {
        int index=find_sensor_index_by_config_file(i ? cfg->vc1_cfg : cfg->vc0_cfg);
        if(index<0 || (ret=configure_sensor(&c->pipes[i],index))!=0) goto fail;
    }
    if((ret=bind_same_mipi_host(c->pipes)) || (ret=check_dual_vc_config(c->pipes))) goto fail;
    if(c->pipes[0].pipe_contex.sensor_config->vin_ochn_attr->vin_basic_attr.format != SENSOR_DATA_TYPE_RAW8 ||
       c->pipes[1].pipe_contex.sensor_config->isp_ochn_attr->fmt != FRM_FMT_NV12 ||
       c->pipes[1].pipe_contex.sensor_config->isp_ochn_attr->bit_width != 8) {ret=-1; goto fail;}
#ifdef X5_RAW_ONLY
    if(profile!=1 || c->pipes[1].pipe_contex.sensor_config->vin_ochn_attr->vin_basic_attr.format!=SENSOR_DATA_TYPE_RAW10 ||
       c->pipes[1].pipe_contex.sensor_config->camera_config->fps!=30) {ret=-1;goto fail;}
    fprintf(stderr,"Raw profile 1: APS input RAW10 nominal fps=30; actual rate must be measured (no ISP)\n");
#endif
    ret=hb_mem_module_open(); if(ret!=0) goto fail;
    c->memory_open=1;
    /* Official sequence: one camera object per VC, probe shared RX only once.
       Do not additionally initialize the toolkit Camera on this path. */
    for(int i=0;i<2;++i) if((ret=create_and_run_vflow(&c->pipes[i]))!=0) goto fail;
    /* Both camera initializations/start sequences must finish before controls.
       Keep the sample's dual-camera ownership until driver semantics are known. */
    fprintf(stderr, "Exposure control stage=after_both_vflows_started profile=%d APS_config=%s sensor_actual=unknown\n", profile, cfg->vc1_cfg);
    #ifndef X5_RAW_ONLY
    ret=isp_set_manual_2a(c->pipes[1].pipe_contex.isp_node_handle, &c->exposure);
    if(ret) goto fail;
    #else
    fprintf(stderr,"Pure VIN: no ISP node or ISP AE created; two independent consumers required\n");
    #endif
    *out=c; return 0;
fail:
    x5_close(c); return ret ? ret : -1;
}
int x5_get(x5_capture *c, int aps, x5_image *out) {
    memset(out,0,sizeof(*out));
#ifdef X5_RAW_ONLY
    hbn_vnode_image_t *image=&c->raw_slots[aps?1:0];
    memset(image,0,sizeof(*image));
#else
    hbn_vnode_image_t *image=calloc(1,sizeof(*image));
#endif
    if(!image) return X5_CAPTURE_FATAL;
    hbn_vnode_handle_t node=aps ? c->pipes[1].pipe_contex.isp_node_handle : c->pipes[0].pipe_contex.vin_node_handle;
#ifdef X5_RAW_ONLY
    node=c->pipes[aps ? 1 : 0].pipe_contex.vin_node_handle;
#endif
    const uint64_t wait_start=capture_ns();
    int ret=hbn_vnode_getframe(node,0,100,image);
    out->wait_ns=capture_ns()-wait_start;
    if(ret!=0) {
#ifndef X5_RAW_ONLY
        free(image);
#endif
        return ret;
    }
    out->lease=image;
#ifndef X5_RAW_ONLY
    if(aps && (image->buffer.format != MEM_PIX_FMT_NV12 || image->buffer.plane_cnt < 2)) {
        fprintf(stderr,"Unexpected ISP output format=%d planes=%d (expected NV12)\n",
                image->buffer.format,image->buffer.plane_cnt);
        x5_release(c,aps,out); return X5_CAPTURE_FATAL;
    }
#endif
    out->width=image->buffer.width; out->height=image->buffer.height;
    out->stride=image->buffer.stride; out->frame_id=image->info.frame_id;
    out->format=image->buffer.format;
    int planes=aps?2:1;
#ifdef X5_RAW_ONLY
    planes=1;
    if(image->buffer.plane_cnt != 1) { x5_release(c,aps,out); return X5_CAPTURE_FATAL; }
#endif
    for(int p=0;p<planes;++p) {
        out->plane[p]=image->buffer.virt_addr[p]; out->size[p]=image->buffer.size[p];
        if(out->plane[p] && out->size[p]) {
            const uint64_t cache_start=capture_ns();
            ret=hb_mem_invalidate_buf_with_vaddr((uint64_t)out->plane[p],out->size[p]);
            out->cache_ns+=capture_ns()-cache_start;
            if(ret!=0) { fprintf(stderr,"Cache invalidation failed: %d\n",ret); x5_release(c,aps,out); return X5_CAPTURE_FATAL; }
        }
    }
    return 0;
}
int x5_release(x5_capture *c, int aps, x5_image *image) {
    if(!image->lease) return 0;
    hbn_vnode_handle_t node=aps ? c->pipes[1].pipe_contex.isp_node_handle : c->pipes[0].pipe_contex.vin_node_handle;
#ifdef X5_RAW_ONLY
    node=c->pipes[aps ? 1 : 0].pipe_contex.vin_node_handle;
#endif
    int ret=hbn_vnode_releaseframe(node,0,(hbn_vnode_image_t*)image->lease);
    if(ret) fprintf(stderr,"hbn_vnode_releaseframe failed: %d\n",ret);
#ifndef X5_RAW_ONLY
    free(image->lease);
#endif
    image->lease=NULL;
    return ret;
}

int x5_get_exposure_state(const x5_capture *c, x5_exposure_state *out) {
    if (!c || !out) return X5_CAPTURE_FATAL;
    *out=c->exposure;
    return 0;
}

int x5_raw_identity(x5_capture *c, int *address, int *mode, int *config_index, int *bus) {
    if(!c) return X5_CAPTURE_FATAL;
    camera_config_t *cfg=c->pipes[1].pipe_contex.sensor_config->camera_config;
    *address=cfg->addr; *mode=cfg->sensor_mode; *config_index=cfg->config_index;
    /* vp_sensor_fixed_mipi_host stores the selected vcon number in csi.index.
       Use the same device-tree bus property as vp_sensors.c, not a guessed bus. */
    *bus=-1;
    char path[128];unsigned char bytes[4];
    snprintf(path,sizeof(path),"/proc/device-tree/soc/cam/vcon@%d/bus",c->pipes[1].pipe_contex.csi_config.index);
    FILE *file=fopen(path,"rb");
    if(file) {
        if(fread(bytes,1,4,file)==4 && bytes[0]==0 && bytes[1]==0 && bytes[2]==0) *bus=bytes[3];
        fclose(file);
    }
    return 0;
}
