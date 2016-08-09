/**
 * This file is part of ffmpeg_ivr
 * 
 * Copyright (C) 2016  OpenSight (www.opensight.cn)
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



#include <float.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <curl/curl.h>

#include "libavutil/avstring.h"
#include "libavutil/opt.h"

#include "libavformat/avformat.h"
    
#include "../cached_segment.h"
#include "../cJSON.h"

#define MIN(a,b) ((a) > (b) ? (b) : (a))

#define HTTP_DEFAULT_RETRY_NUM    2

static int http_status_to_av_code(int status_code)
{
    if(status_code == 400){
        return AVERROR_HTTP_BAD_REQUEST;
    }else if(status_code == 404){
        return AVERROR_HTTP_NOT_FOUND;
    }else if (status_code > 400 && status_code < 500){
        return AVERROR_HTTP_OTHER_4XX;
    }else if ( status_code >= 500 && status_code < 600){
        return AVERROR_HTTP_SERVER_ERROR;
    }else{
        return AVERROR_UNKNOWN;
    }    
}


#define  IVR_NAME_FIELD_KEY  "name"
#define  IVR_URI_FIELD_KEY  "uri"
#define  IVR_ERR_INFO_FIELD_KEY "info"

#define MAX_HTTP_RESULT_SIZE  8192

#define ENABLE_CURLOPT_VERBOSE


typedef struct HttpBuf{
    char * buf;
    int buf_size; 
    int pos;    
}HttpBuf;

static size_t http_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    int data_size = size * nmemb;
    HttpBuf * http_buf = (HttpBuf *)userdata;
        
    if(data_size > (http_buf->buf_size - http_buf->pos)){
        return 0;
    }
    memcpy(http_buf->buf + http_buf->pos, ptr, data_size);
    http_buf->pos += data_size;
    return data_size;    
}

static size_t http_read_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpBuf * http_buf = (HttpBuf *)userdata;
    int data_size = MIN(size * nmemb, http_buf->buf_size - http_buf->pos);    
    
    memcpy(ptr, http_buf->buf + http_buf->pos, data_size);
    http_buf->pos += data_size;
    return data_size;
}

static int http_post(char * http_uri, 
                     int32_t io_timeout,  //in milli-seconds 
                     char * post_content_type, 
                     char * post_data, int post_len,
                     int32_t retries,
                     int * status_code,
                     char * result_buf, int *buf_size)
{
    CURL * easyhandle = NULL;
    int ret = 0;
    struct curl_slist *headers=NULL;
    char content_type_header[128];
    long status;
    HttpBuf http_buf;
    char err_buf[CURL_ERROR_SIZE] = "unknown";
    CURLcode curl_res = CURLE_OK;

    
    if(retries <= 0){
        retries =  HTTP_DEFAULT_RETRY_NUM;       
    }


    if(post_content_type != NULL){
        memset(content_type_header, 0, 128);
        snprintf(content_type_header, 127, 
                 "Content-Type: %s", post_content_type);
        headers = curl_slist_append(headers, content_type_header);

    }   
        
    while(retries-- > 0){
        memset(&http_buf, 0, sizeof(HttpBuf));   
        ret = 0;
        strcpy(err_buf, "unknown");
        
        easyhandle = curl_easy_init();
        if(easyhandle == NULL){
            ret = AVERROR(ENOMEM);
            break;
        }

        if(curl_easy_setopt(easyhandle, CURLOPT_URL, http_uri)){
            ret = AVERROR_EXTERNAL;
            break;                
        }   
        
        if(headers != NULL){
            if(curl_easy_setopt(easyhandle, CURLOPT_HTTPHEADER, headers)){
                ret = AVERROR_EXTERNAL;
                break;                
            }            
        }
        if(curl_easy_setopt(easyhandle, CURLOPT_POSTFIELDS, post_data)){
            ret = AVERROR_EXTERNAL;
            break;            
        }  
        if(curl_easy_setopt(easyhandle, CURLOPT_POSTFIELDSIZE, post_len)){
            ret = AVERROR_EXTERNAL;
            break;              
        }  

        if(io_timeout > 0){
            if(curl_easy_setopt(easyhandle, CURLOPT_TIMEOUT_MS , io_timeout)){
                ret = AVERROR_EXTERNAL;
                break;                
            }
        } 
        if(result_buf != NULL && buf_size != NULL && (*buf_size) != 0){
            http_buf.buf = result_buf;
            http_buf.buf_size = (*buf_size);
            http_buf.pos = 0;
            
            if(curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, http_write_callback)){
                ret = AVERROR_EXTERNAL;
                break;                
            }
            if(curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &http_buf)){
                ret = AVERROR_EXTERNAL;
                break;                
            }
        }  

        if(curl_easy_setopt(easyhandle, CURLOPT_ERRORBUFFER, err_buf)){
            ret = AVERROR_EXTERNAL;
            break;                
        }
 #ifdef ENABLE_CURLOPT_VERBOSE   
        if(curl_easy_setopt(easyhandle, CURLOPT_VERBOSE, 1)){
            ret = AVERROR_EXTERNAL;
            break;                
        }    
 #endif
        
        if((curl_res = curl_easy_perform(easyhandle)) != CURLE_OK){
            ret = AVERROR_EXTERNAL;            
            if(curl_res == CURLE_OPERATION_TIMEDOUT ){
                break;
            }else{
                //retry
                curl_easy_cleanup(easyhandle);  
                easyhandle = NULL;
                continue;
            }
        }
    
        if(curl_easy_getinfo(easyhandle, CURLINFO_RESPONSE_CODE, &status)){
            ret = AVERROR_EXTERNAL;
            break;
        }    
        
        if(status_code){
            *status_code = status;
        }
        if(buf_size != NULL){
            (*buf_size) = http_buf.pos;
        }

        break;   // successful, then exit the loop
    }//while(retries-- > 0){
    

    
fail:    
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP POST failed:%s\n", err_buf);        
    }
    if(headers != NULL){
        curl_slist_free_all(headers);
        headers = NULL;
    }
    if(easyhandle != NULL){
        curl_easy_cleanup(easyhandle);   
        easyhandle = NULL;
    }
    return ret;
}

static int http_put(char * http_uri, 
                    int32_t io_timeout,  //in milli-seconds 
                    char * content_type, 
                    char * buf, int buf_size,
                    int32_t retries,
                    int * status_code)
{
    CURL * easyhandle = NULL;
    int ret = 0;
    struct curl_slist *headers=NULL;
    char content_type_header[128];
    char expect_header[128];
    long status;
    HttpBuf http_buf;
    char err_buf[CURL_ERROR_SIZE] = "unknown";   
    CURLcode curl_res = CURLE_OK; 
    
    if(retries <= 0){
        retries =  HTTP_DEFAULT_RETRY_NUM;       
    }    


    if(content_type != NULL){
        memset(content_type_header, 0, 128);
        snprintf(content_type_header, 127, 
                 "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, content_type_header);
    }   
    //disable "Expect: 100-continue"  header
    memset(expect_header, 0, 128);
    strcpy(expect_header, "Expect:");
    headers = curl_slist_append(headers, expect_header);   

    while(retries-- > 0){
        memset(&http_buf, 0, sizeof(HttpBuf));
        ret = 0;
        strcpy(err_buf, "unknown");
    
    
        easyhandle = curl_easy_init();
        if(easyhandle == NULL){
            ret = AVERROR(ENOMEM);
            break;
        }

        if(curl_easy_setopt(easyhandle, CURLOPT_URL, http_uri)){
            ret = AVERROR_EXTERNAL;
            break;             
        }   
        
        if(curl_easy_setopt(easyhandle, CURLOPT_UPLOAD, 1L)){
            ret = AVERROR_EXTERNAL;
            break;              
        }   
    
  

       if(curl_easy_setopt(easyhandle, CURLOPT_HTTPHEADER, headers)){
            ret = AVERROR_EXTERNAL;
            break;               
        }
       
        if(curl_easy_setopt(easyhandle, CURLOPT_INFILESIZE, buf_size)){
            ret = AVERROR_EXTERNAL;
            break;              
        }    

        if(io_timeout > 0){
            if(curl_easy_setopt(easyhandle, CURLOPT_TIMEOUT_MS , io_timeout)){
                ret = AVERROR_EXTERNAL;
                break;               
            }
        }   
    
        if(buf != NULL && buf_size != 0){
            http_buf.buf = buf;
            http_buf.buf_size = buf_size;
            http_buf.pos = 0;
            
            if(curl_easy_setopt(easyhandle, CURLOPT_READFUNCTION, http_read_callback)){
                ret = AVERROR_EXTERNAL;
                break;                
            }
            if(curl_easy_setopt(easyhandle, CURLOPT_READDATA, &http_buf)){
                ret = AVERROR_EXTERNAL;
                break;               
            }
        }
    

        if(curl_easy_setopt(easyhandle, CURLOPT_ERRORBUFFER, err_buf)){
            ret = AVERROR_EXTERNAL;
            break;               
        }

        
#ifdef ENABLE_CURLOPT_VERBOSE   
        if(curl_easy_setopt(easyhandle, CURLOPT_VERBOSE, 1)){
            ret = AVERROR_EXTERNAL;
            break;                
        }    
#endif
        
        if((curl_res = curl_easy_perform(easyhandle)) != CURLE_OK){
            ret = AVERROR_EXTERNAL;            
            if(curl_res == CURLE_OPERATION_TIMEDOUT ){
                break;
            }else{
                //retry
                curl_easy_cleanup(easyhandle);  
                easyhandle = NULL;
                continue;
            }
        }
    
        if(curl_easy_getinfo(easyhandle, CURLINFO_RESPONSE_CODE, &status)){
            ret = AVERROR_EXTERNAL;
            break;
        }    
        
        if(status_code){
            *status_code = status;
        }
        
        break;   // successful, then exit the loop
    }//while(retries-- > 0){    
fail:    

    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP PUT failed:%s\n", err_buf);        
    }

    if(headers != NULL){
        curl_slist_free_all(headers);
    }
    
    if(easyhandle != NULL){
        curl_easy_cleanup(easyhandle);   
        easyhandle = NULL;
    }  
    
    return ret;
}


static int create_file(char * ivr_rest_uri, 
                       int32_t io_timeout, 
                       CachedSegment *segment, 
                       char * filename, int filename_size,
                       char * file_uri, int file_uri_size)
{
    //uint8_t checksum[16];
    //char checksum_b64[32];
    //char checksum_b64_escape[128];
    char post_data_str[256];
    char * http_response_json = av_mallocz(MAX_HTTP_RESULT_SIZE);
    cJSON * json_root = NULL;
    cJSON * json_name = NULL;
    cJSON * json_uri = NULL;    
    cJSON * json_info = NULL;        
    int ret;
    int status_code = 200;
    int response_size = MAX_HTTP_RESULT_SIZE - 1;
    
    if(filename_size){
        filename[0] = 0;
    }
    if(file_uri_size){
        file_uri[0] = 0;
    }    
    
    //prepare post_data
    //av_md5_sum(checksum, segment->buffer, segment->size);
    //av_base64_encode(checksum_b64, 32, checksum, 16);
    //url_encode(checksum_b64_escape, checksum_b64);
    sprintf(post_data_str, 
            "op=create&content_type=video%%2Fmp2t&size=%d&start=%.6f&duration=%.6f",
            segment->size,
            segment->start_ts, 
            segment->duration);
        
    //issue HTTP request
    ret = http_post(ivr_rest_uri, 
                    io_timeout,
                    NULL, 
                    post_data_str, strlen(post_data_str), 
                    HTTP_DEFAULT_RETRY_NUM,
                    &status_code,
                    http_response_json, &response_size);
    if(ret){
        goto failed;       
    }

    //parse the result
    if(status_code >= 200 && status_code < 300){
        json_root = cJSON_Parse(http_response_json);
        if(json_root== NULL){
            ret = AVERROR(EINVAL);
            av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP response Json parse failed(%s)\n", http_response_json);
            goto failed;
        }
        json_name = cJSON_GetObjectItem(json_root, IVR_NAME_FIELD_KEY);
        if(json_name && json_name->type == cJSON_String && json_name->valuestring){
            av_strlcpy(filename, json_name->valuestring, filename_size);
        }
        json_uri = cJSON_GetObjectItem(json_root, IVR_URI_FIELD_KEY);
        if(json_uri && json_uri->type == cJSON_String && json_uri->valuestring){
            av_strlcpy(file_uri, json_uri->valuestring, file_uri_size);
        }
    }else{

        ret = http_status_to_av_code(status_code);
        if(response_size != 0){
            json_root = cJSON_Parse(http_response_json);
            if(json_root== NULL){
                av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP response Json parse failed(%s)\n", http_response_json);   
            }else{
                json_info = cJSON_GetObjectItem(json_root, IVR_ERR_INFO_FIELD_KEY);
                if(json_info && json_info->type == cJSON_String && json_info->valuestring){            
                    av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP create file status code(%d):%s\n", 
                           status_code, json_info->valuestring);
                }        
            }//if(json_root== NULL)
            
        }//if(response_size != 0)
        goto failed;
        
    }
    

failed:
    if(json_root){
        cJSON_Delete(json_root); 
        json_root = NULL;
    }
    av_free(http_response_json);  
        
    return ret;
}

static int upload_file(CachedSegment *segment, 
                       int32_t io_timeout, 
                       char * file_uri)
{
    int status_code = 200;
    int ret = 0;  
    ret = http_put(file_uri, io_timeout, "video/mp2t",
                   segment->buffer, segment->size, 
                   HTTP_DEFAULT_RETRY_NUM,
                   &status_code);
    if(ret){
        return ret;
    }
    
    if(status_code < 200 || status_code >= 300){
        ret = http_status_to_av_code(status_code);
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] http upload file failed with status(%d)\n", 
                   status_code);       
        goto fail;
    }
    return 0;
fail:
    return ret;
}

static int save_file(char * ivr_rest_uri,
                      int32_t io_timeout,
                      CachedSegment *segment, 
                      char * filename,
                      int success)
{
    char post_data_str[512];  
    int status_code = 200;
    int ret = 0;
    char * http_response_json = av_mallocz(MAX_HTTP_RESULT_SIZE);
    cJSON * json_root = NULL;
    cJSON * json_info = NULL; 
    int response_size = MAX_HTTP_RESULT_SIZE - 1;    
    
    //prepare post_data
    if(success){
        sprintf(post_data_str, "op=save&name=%s&size=%d&start=%.6f&duration=%.6f",
                filename,
                segment->size,
                segment->start_ts, 
                segment->duration);
                
    }else{
        sprintf(post_data_str, "op=fail&name=%s&size=%d&start=%.6f&duration=%.6f",
                filename,
                segment->size,
                segment->start_ts, 
                segment->duration);        
    }

    //issue HTTP request
    ret = http_post(ivr_rest_uri, 
                    io_timeout,
                    NULL, 
                    post_data_str, strlen(post_data_str), 
                    HTTP_DEFAULT_RETRY_NUM,
                    &status_code,
                    http_response_json, &response_size); 
    if(ret){
        goto failed;
    }



    if(status_code < 200 || status_code >= 300){

        ret = http_status_to_av_code(status_code);
        if(response_size != 0){
            json_root = cJSON_Parse(http_response_json);
            if(json_root== NULL){
                av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP response Json parse failed(%s)\n", http_response_json);       
            }else{
                json_info = cJSON_GetObjectItem(json_root, IVR_ERR_INFO_FIELD_KEY);
                if(json_info && json_info->type == cJSON_String && json_info->valuestring){
                    av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP create file status code(%d):%s\n", 
                       status_code, json_info->valuestring);
                }        
            }
        }
        
        goto failed;
      
    }

failed:
    if(json_root){
        cJSON_Delete(json_root); 
        json_root = NULL;
    }
    av_free(http_response_json);  
        
    return ret;
}




static int ivr_init(CachedSegmentContext *cseg)
{
    //init curl lib
    curl_global_init(CURL_GLOBAL_ALL);    

    
    return 0;
}


#define MAX_FILE_NAME 1024
#define MAX_URI_LEN 1024

#define FILE_CREATE_TIMEOUT 10000
static int ivr_write_segment(CachedSegmentContext *cseg, CachedSegment *segment)
{
    char ivr_rest_uri[MAX_URI_LEN] = "http";
    char file_uri[MAX_URI_LEN];
    char filename[MAX_FILE_NAME];
    char *p;
    int ret = 0;

    if(cseg->filename == NULL || strlen(cseg->filename) == 0){
        ret = AVERROR(EINVAL);
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] http filename absent\n");          
        goto fail;       
    }
    
    if(strlen(cseg->filename) > (MAX_URI_LEN - 5)){
        ret = AVERROR(EINVAL);
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] filename is too long\n");          
        goto fail;
    }

    p = strchr(cseg->filename, ':');  
    if(p){
        av_strlcat(ivr_rest_uri, p, MAX_URI_LEN);
    }else{
        ret = AVERROR(EINVAL);
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] filename malformat\n");
        goto fail;
    }
    
    //get URI of the file for segment
    ret = create_file(ivr_rest_uri, 
                      FILE_CREATE_TIMEOUT,
                      segment, 
                      filename, MAX_FILE_NAME,
                      file_uri, MAX_URI_LEN);
                      
    if(ret){
        goto fail;
    }
   
    if(strlen(filename) == 0 || strlen(file_uri) == 0){
        ret = 1; //cannot upload at the moment
    }else{    
        //upload segment to the file URI
        ret = upload_file(segment, 
                          cseg->writer_timeout,
                          file_uri);                      
        if(ret == 0){
            //save the file info to IVR db
            ret = save_file(ivr_rest_uri, 
                            FILE_CREATE_TIMEOUT,
                            segment, filename, 1);

        }else{
            //fail the file, remove it from IVR
            ret = save_file(ivr_rest_uri, 
                            FILE_CREATE_TIMEOUT,
                            segment, filename, 0);
    
        }//if(ret == 0){
            
        if(ret){
            goto fail;
        } 
    }  

fail:
   
    return ret;
}

static void ivr_uninit(CachedSegmentContext *cseg)
{
    curl_global_cleanup();
}


CachedSegmentWriter cseg_ivr_writer = {
    .name           = "ivr_writer",
    .long_name      = "IVR cloud storage segment writer", 
    .protos         = "ivr", 
    .init           = ivr_init, 
    .write_segment  = ivr_write_segment, 
    .uninit         = ivr_uninit,
};

