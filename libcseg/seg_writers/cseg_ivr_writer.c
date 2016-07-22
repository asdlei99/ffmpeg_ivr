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
    
#include "../min_cached_segment.h"
#include "../utils/cJSON.h"
#include "../utils/http_client/HTTPClient.h"

#define MIN(a,b) ((a) > (b) ? (b) : (a))


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
                     int * status_code,
                     char * result_buf, int max_buf_size)
{
    CURL * easyhandle = NULL;
    int ret = 0;
    struct curl_slist *headers=NULL;
    char content_type_header[128];
    long status;
    HttpBuf http_buf;
    char err_buf[CURL_ERROR_SIZE] = "unknown";

    int32_t                 status_code;
    HTTP_CLIENT             HTTPClient;
    HTTP_SESSION_HANDLE     pHTTP = 0;
    int32_t                  nSize = 0,nTotal = 0;
    
    
    pHTTP = HTTPClientOpenRequest(0);
    if(!pHTTP){
        ret = -HTTP_CLIENT_ERROR_NO_MEMORY;    
        goto fail;           
    }
    
    
    memset(&http_buf, 0, sizeof(HttpBuf));

    if((ret = HTTPClientSetVerb(pHTTP,VerbPost)) != HTTP_CLIENT_SUCCESS)
    {
        ret = -ret;
        goto fail;
    }    
    if(post_content_type != NULL){
        if((ret = HTTPClientAddRequestHeaders(pHTTP, "Content-Type", post_content_type, 0)) != HTTP_CLIENT_SUCCESS)
        {
            ret = -ret;
            goto fail;
        }
    }else{
        if((ret = HTTPClientAddRequestHeaders(pHTTP, 
                                              "Content-Type", 
                                              "application/x-www-form-urlencoded", 0)) != HTTP_CLIENT_SUCCESS)
        {
            ret = -ret;
            goto fail;
        }        
    }

    if((ret = HTTPClientSendRequest(pHTTP, http_uri, post_data,
                post_len,TRUE, io_timeout, 0)) != HTTP_CLIENT_SUCCESS)
    {
        ret = -ret
        goto fail;
    }     
    

    // Retrieve the the headers and analyze them
    if((ret = HTTPClientRecvResponse(pHTTP,io_timeout)) != HTTP_CLIENT_SUCCESS)
    {
        ret = -ret
        goto fail;
    }
    HTTPClientGetInfo(pHTTP, &HTTPClient);
    if(status_code){
        *status_code = HTTPClient.HTTPStatusCode;
    }    
        

    // Get the data until we get an error or end of stream code
    // printf("Each dot represents %d bytes:\n",HTTP_BUFFER_SIZE );
    nTotal = 0;
    nSize = 0;
    ret = HTTP_CLIENT_SUCCESS;
    while(ret == HTTP_CLIENT_SUCCESS)
    {
        if(nTotal >= HTTPClient.TotalResponseBodyLength){ 
           
            break;
        }
        if(nTotal >= max_buf_size){
            ret = HTTP_CLIENT_ERROR_NO_MEMORY;
            break;
        }
            
        // Set the size of our buffer
        nSize = max_buf_size - nTotal;   

        // Get the data
        ret = HTTPClientReadData(pHTTP,result_buf+nTotal,nSize,io_timeout,&nSize);
        nTotal += nSize;

    }
    if(ret == HTTP_CLIENT_EOS){
        ret = 0;
    }


fail:    
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP POST failed(%d)\n", ret);        
    }
    
    if(pHTTP){
        HTTPClientCloseRequest(&pHTTP);
        pHTTP = 0;
    }
  
    return ret;
}

static int http_put(char * http_uri, 
                    int32_t io_timeout,  //in milli-seconds 
                    char * content_type, 
                    char * buf, int buf_size,
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
    
    memset(&http_buf, 0, sizeof(HttpBuf));
    
    
    easyhandle = curl_easy_init();
    if(easyhandle == NULL){
        return AVERROR(ENOMEM);
    }

    if(curl_easy_setopt(easyhandle, CURLOPT_URL, http_uri)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
    }   
    
    if(curl_easy_setopt(easyhandle, CURLOPT_UPLOAD, 1L)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
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

    if(curl_easy_setopt(easyhandle, CURLOPT_HTTPHEADER, headers)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
    }
   
    if(curl_easy_setopt(easyhandle, CURLOPT_INFILESIZE, buf_size)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
    }    

    if(io_timeout > 0){
        if(curl_easy_setopt(easyhandle, CURLOPT_TIMEOUT_MS , io_timeout)){
            ret = AVERROR_EXTERNAL;
            goto fail;                
        }
    }   
    
    if(buf != NULL && buf_size != 0){
        http_buf.buf = buf;
        http_buf.buf_size = buf_size;
        http_buf.pos = 0;
        
        if(curl_easy_setopt(easyhandle, CURLOPT_READFUNCTION, http_read_callback)){
            ret = AVERROR_EXTERNAL;
            goto fail;                
        }
        if(curl_easy_setopt(easyhandle, CURLOPT_READDATA, &http_buf)){
            ret = AVERROR_EXTERNAL;
            goto fail;                
        }
    }
    

    if(curl_easy_setopt(easyhandle, CURLOPT_ERRORBUFFER, err_buf)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
    }

    
 #ifdef ENABLE_CURLOPT_VERBOSE   
    if(curl_easy_setopt(easyhandle, CURLOPT_VERBOSE, 1)){
        ret = AVERROR_EXTERNAL;
        goto fail;                
    }    
 #endif
        
    if(curl_easy_perform(easyhandle)){
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    
    if(curl_easy_getinfo(easyhandle, CURLINFO_RESPONSE_CODE, &status)){
        ret = AVERROR_EXTERNAL;
        goto fail;
    }    
    
    if(status_code){
        *status_code = status;
    }
    
fail:    

    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP PUT failed:%s\n", err_buf);        
    }

    if(headers != NULL){
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(easyhandle);    
    
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
    char * http_response_json = av_malloc(MAX_HTTP_RESULT_SIZE);
    cJSON * json_root = NULL;
    cJSON * json_name = NULL;
    cJSON * json_uri = NULL;    
    cJSON * json_info = NULL;        
    int ret;
    int status_code = 200;
    
    if(filename_size){
        filename[0] = 0;
    }
    if(file_uri_size){
        file_uri[0] = 0;
    }    
    
    memset(http_response_json, 0, MAX_HTTP_RESULT_SIZE);
    
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
                    &status_code,
                    http_response_json, MAX_HTTP_RESULT_SIZE - 1);
    if(ret < 0){
        goto failed;       
    }
    ret = 0;

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
        json_root = cJSON_Parse(http_response_json);
        if(json_root== NULL){
            av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP response Json parse failed(%s)\n", http_response_json);   
            goto failed;
        }
        json_info = cJSON_GetObjectItem(json_root, IVR_ERR_INFO_FIELD_KEY);
        if(json_info && json_info->type == cJSON_String && json_info->valuestring){            
            av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP create file status code(%d):%s\n", 
                   status_code, json_info->valuestring);
            goto failed;
        }        
        
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
                   &status_code);
    if(ret < 0){
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
                      char * filename)
{
    char post_data_str[512];  
    int status_code = 200;
    int ret = 0;
    char * http_response_json = av_mallocz(MAX_HTTP_RESULT_SIZE);
    cJSON * json_root = NULL;
    cJSON * json_info = NULL;     
    
    //prepare post_data
    sprintf(post_data_str, "op=save&name=%s&size=%d&start=%.6f&duration=%.6f",
            filename,
            segment->size,
            segment->start_ts, 
            segment->duration);

    //issue HTTP request
    ret = http_post(ivr_rest_uri, 
                    io_timeout,
                    NULL, 
                    post_data_str, strlen(post_data_str), 
                    &status_code,
                    http_response_json, MAX_HTTP_RESULT_SIZE - 1); 
    if(ret < 0){
        return ret;
    }
    ret = 0;


    if(status_code < 200 || status_code >= 300){

        ret = http_status_to_av_code(status_code);
        
        json_root = cJSON_Parse(http_response_json);
        if(json_root== NULL){
            av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP response Json parse failed(%s)\n", http_response_json);       
        }else{
            json_info = cJSON_GetObjectItem(json_root, IVR_ERR_INFO_FIELD_KEY);
            if(json_info && json_info->type == cJSON_String && json_info->valuestring){
                av_log(NULL, AV_LOG_ERROR,  "[cseg_ivr_writer] HTTP create file status code(%d):%s\n", 
                   status_code, json_info->valuestring);
            }        
            cJSON_Delete(json_root);             
        }
      
    }

    av_free(http_response_json);

    return ret;
}



static int ivr_init(CachedSegmentContext *cseg)
{
    //init curl lib
    //curl_global_init(CURL_GLOBAL_ALL);    

    
    return 0;
}


#define MAX_FILE_NAME 1024
#define MAX_URI_LEN 1024

#define FILE_CREATE_TIMEOUT 10
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
        strncat(ivr_rest_uri, p, MAX_URI_LEN);
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
        if(ret){
            goto fail;
        }    
        
        //save the file info to IVR db
        ret = save_file(ivr_rest_uri, 
                        FILE_CREATE_TIMEOUT,
                        segment, filename);
        if(ret){
            goto fail;
        }  
    }  

fail:
   
    return ret;
}

static void ivr_uninit(CachedSegmentContext *cseg)
{
    //curl_global_cleanup();
}


CachedSegmentWriter cseg_ivr_writer = {
    .name           = "ivr_writer",
    .long_name      = "IVR cloud storage segment writer", 
    .protos         = "ivr", 
    .init           = ivr_init, 
    .write_segment  = ivr_write_segment, 
    .uninit         = ivr_uninit,
};

