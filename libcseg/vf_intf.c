/**
 * This file is part of libcseg library, which belongs to ffmpeg_ivr
 * project. 
 * 
 * Copyright (C) 2014  OpenSight (www.opensight.cn)
 * 
 * ffmpeg_ivr is an extension of ffmpeg to implements the new feature for IVR
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <stdint.h>

#include "vf_intf.h"

#include "min_cached_segment.h"


static double start_ts = -1;
static int32_t io_timeout = 20000;   /*20 sec */
static uint64_t start_sequence = 0; 

#define AAC_SAMPLE_PER_FRAME   1024

#define MIN_TIMESTAMP   (time_t)1000000000

#define START_PTS       (int64_t)90000

#define PTS_MS_200      18000
#define PTS_MS_400      36000

int vf_init_cseg_muxer(const char * filename,
                       av_stream_t* streams, uint8_t stream_count,
                       double segment_time,
                       int max_nb_segments,
                       uint32_t max_seg_size, 
                       double pre_recoding_time, 
                       CachedSegmentContext **cseg)
{
    vf_private * vf = NULL;
    int i;
    int ret = 0;
    int video_index = -1;

    
    if(stream_count > MAX_STREAM_NUM){
        cseg_log(CSEG_LOG_ERROR,
               "stream count is over the limitation\n");
        return CSEG_ERROR(EINVAL);          
    }
    
    vf = cseg_malloc(sizeof(vf_private));
    memset(vf, 0, sizeof(vf_private));
    
    vf.is_started = 0;
    vf.start_ts = -1.0;
    vf.audio_stream_index = -1;
    vf.stream_cout = stream_count;
    for(i=0;i<stream_count;i++){
        if(streams[i].type == AV_STREAM_TYPE_VIDEO){
            video_index = i;
        }else if(streams[i].type == AV_STREAM_TYPE_AUDIO){
            vf.audio_stream_index = i;            
        }
        
        vf.stream_last_pts[i] = NOPTS_VALUE;

    }
    if(video_index < 0){
        cseg_log(CSEG_LOG_ERROR,
               "video stream absent\n");
        return CSEG_ERROR(EINVAL);    
    }
    
    
    ret = init_cseg_muxer(filename,
                          streams, stream_count,
                          start_sequence,
                          segment_time, 
                          max_nb_segments,
                          pre_recoding_time,
                          start_ts,
                          start_sequence,
                          vf,
                          cseg);
    if(ret){
        goto fail;
    }
    
    return 0;

fail:
    if(vf){
        cseg_free(vf);
        vf = NULL;
    }
    return ret;
}


int vf_cseg_sendAV(CachedSegmentContext *cseg, 
                   int stream_index,                   
                   uint8_t* frame_data, 
                   uint32_t frame_len, 
                   int codec_type, int frm_rate, 
                   int key)
{
    vf_private * vf = (vf_private *)get_cseg_muxer_private(cseg);
    av_stream_t *streams = (av_stream_t *)cseg->streams;
    int ret = 0;
    av_packet_t pkt;
    
    if(stream_index > cseg->stream_count){
        cseg_log(CSEG_LOG_ERROR,
               "stream_index invalid\n");
        ret = CSEG_ERROR(EINVAL);
        return ret;        
    }
    
    if(streams[stream_index].codec != codec_type){
        cseg_log(CSEG_LOG_ERROR,
               "codec type error\n");
        ret = CSEG_ERROR(EINVAL);
        return ret;
    }
    
    //check start    
    if(!vf->is_started){
        //check now        
        
        if(streams[stream_index].type == AV_STREAM_TYPE_VIDEO && key){
            struct timeval now;
            struct timespec now_tp;
            gettimeofday(&now, NULL);
            if(now.tv_sec > MIN_TIMESTAMP){                
                ret = clock_gettime(CLOCK_MONOTONIC, &now_tp);
                if(ret){
                    cseg_log(CSEG_LOG_ERROR,
                           "clock_gettime failed\n");
                    ret = CSEG_ERROR(errno);
                    return ret;    
                }
                vf->is_started = 1;
                vf->start_tp = (double)now_tp.tv_sec + (double)now_tp.nsec / 1000000000.0;
            }
        }
        
    }
    if(!vf->is_started){
        //stream not start, ignore this packet
        return 0;
    }
    
    //construct packet
    memset(&pkt, 0, sizeof(av_packet_t));
    pkt.av_stream_index = stream_index;
    if(key){
        pkt.flags |= AV_PACKET_FLAGS_KEY; 
    }
    pkt.data = frame_data;
    pkt.size = frame_len;
    pkt.dts = NOPTS_VALUE;
    
    if(streams[stream_index].codec == AV_STREAM_CODEC_AAC_WITH_ADTS){
        //trick: hack the frame data for correctness
    }
        
    //calculate pts
    if(vf->stream_last_pts[stream_index] == NOPTS_VALUE){
        struct timespec now_tp;
        double now_d;
        ret = clock_gettime(CLOCK_MONOTONIC, &now_tp);
        if(ret){
            cseg_log(CSEG_LOG_ERROR,
                   "clock_gettime failed\n");
            ret = CSEG_ERROR(errno);
            return ret;            
        }
        now_d = (double)now_tp.tv_sec + (double)now_tp.nsec / 1000000000.0;
        pkt.pts = (now_d - vf->start_tp) * TS_TIME_BASE + START_PTS;
        
    }else{
        if(streams[stream_index].type == AV_STREAM_TYPE_VIDEO){
            pkt.pts = vf->stream_last_pts[stream_index] + (int64_t)TS_TIME_BASE / frame_rate;
            if(vf->audio_stream_index != -1 && 
               vf->stream_last_pts[vf->audio_stream_index] != NOPTS_VALUE){
                //sync PTS with audio 
                if(pkt.pts + PTS_MS_200 < vf->stream_last_pts[vf->audio_stream_index]){
                    pkt.pts = vf->stream_last_pts[vf->audio_stream_index];
                }else if(pkt.pts > vf->stream_last_pts[vf->audio_stream_index] +  PTS_MS_400){
                    frame_rate <<= 4;
                }else if(pkt.pts > vf->stream_last_pts[vf->audio_stream_index] +  PTS_MS_200){
                    frame_rate <<= 2;
                }                   
            }
        }else{
            if(streams[stream_index].codec == AV_STREAM_CODEC_AAC ||
               streams[stream_index].codec == AV_STREAM_CODEC_AAC_WITH_ADTS){
                pkt.pts = vf->stream_last_pts[stream_index] + 
                          (int64_t)(TS_TIME_BASE * AAC_SAMPLE_PER_FRAME) / frame_rate;   
            }else{
                cseg_log(CSEG_LOG_ERROR,
                       "audio codec %d not support\n", streams[stream_index].codec);
                ret = CSEG_ERROR(EPFNOSUPPORT);
                return ret;                   
            }
            
        }
    }
    
    return cseg_write_packet(cseg, &pkt);    
                       
}


void vf_release_cseg_muxer(CachedSegmentContext *cseg)
{
    vf_private * vf = (vf_private *)get_cseg_muxer_private(cseg);
    release_cseg_muxer(cseg);
    if(vf){
        cseg_free(vf);
    }
    
}

