/*
 * Copyright 2021 Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <ctime>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <fstream>
#include <signal.h>
#include <errno.h>
#include "nlsClient.h"
#include "nlsEvent.h"
#include "nlsToken.h"
#include "speechTranscriberRequest.h"
#include "profile_scan.h"

#define SELF_TESTING_TRIGGER
#define FRAME_16K_20MS 640
#define FRAME_16K_100MS 3200
#define FRAME_8K_20MS 320
#define SAMPLE_RATE_8K 8000
#define SAMPLE_RATE_16K 16000

#define OPERATION_TIMEOUT_S 5
#define LOOP_TIMEOUT 60
#define DEFAULT_STRING_LEN 128

// 自定义线程参数
struct ParamStruct {
  char fileName[DEFAULT_STRING_LEN];
  char token[DEFAULT_STRING_LEN];
  char appkey[DEFAULT_STRING_LEN];
  char url[DEFAULT_STRING_LEN];

  uint64_t startedConsumed;   /*started事件完成次数*/
  uint64_t firstConsumed;     /*首包完成次数*/
  uint64_t completedConsumed; /*completed事件次数*/
  uint64_t closeConsumed;     /*closed事件次数*/

  uint64_t failedConsumed;    /*failed事件次数*/
  uint64_t requestConsumed;   /*发起请求次数*/

  uint64_t sendConsumed;      /*sendAudio调用次数*/

  uint64_t startTotalValue;   /*所有started完成时间总和*/
  uint64_t startAveValue;     /*started完成平均时间*/
  uint64_t startMaxValue;     /*调用start()到收到started事件最大用时*/
  uint64_t startMinValue;     /*调用start()到收到started事件最小用时*/

  uint64_t firstTotalValue;   /*所有收到首包用时总和*/
  uint64_t firstAveValue;     /*收到首包平均时间*/
  uint64_t firstMaxValue;     /*调用start()到收到首包最大用时*/
  uint64_t firstMinValue;     /*调用start()到收到首包最小用时*/
  bool     firstFlag;         /*是否收到首包的标记*/

  uint64_t endTotalValue;     /*start()到completed事件的总用时*/
  uint64_t endAveValue;       /*start()到completed事件的平均用时*/
  uint64_t endMaxValue;       /*start()到completed事件的最大用时*/
  uint64_t endMinValue;       /*start()到completed事件的最小用时*/

  uint64_t closeTotalValue;   /*start()到closed事件的总用时*/
  uint64_t closeAveValue;     /*start()到closed事件的平均用时*/
  uint64_t closeMaxValue;     /*start()到closed事件的最大用时*/
  uint64_t closeMinValue;     /*start()到closed事件的最小用时*/

  uint64_t sendTotalValue;    /*单线程调用sendAudio总耗时*/

  uint64_t audioFileTimeLen;  /*灌入音频文件的音频时长*/

  uint64_t s50Value;          /*start()到started用时50ms以内*/
  uint64_t s100Value;         /*start()到started用时100ms以内*/
  uint64_t s200Value;
  uint64_t s500Value;
  uint64_t s1000Value;
  uint64_t s2000Value;

  pthread_mutex_t mtx;
};

struct SentenceParamStruct {
  uint32_t sentenceId;
  std::string text;
  uint64_t beginTime;
  uint64_t endTime;
  uint64_t beginTv;
  struct timeval endTv;
};

// 自定义事件回调参数
class ParamCallBack {
 public:
  ParamCallBack(ParamStruct* param) {
    tParam = param;

    pthread_mutex_init(&mtxWord, NULL);
    pthread_cond_init(&cvWord, NULL);
  };
  ~ParamCallBack() {
    tParam = NULL;
    pthread_mutex_destroy(&mtxWord);
    pthread_cond_destroy(&cvWord);
  };

  unsigned long userId;
  char userInfo[8];

  pthread_mutex_t mtxWord;
  pthread_cond_t cvWord;

  struct timeval startTv;
  struct timeval startedTv;
  struct timeval startAudioTv;
  struct timeval firstTv;
  struct timeval completedTv;
  struct timeval closedTv;
  struct timeval failedTv;

  ParamStruct* tParam;

  std::vector<struct SentenceParamStruct> sentenceParam;
};

// 统计参数
struct ParamStatistics {
  bool running;
  bool success_flag;
  bool failed_flag;

  uint64_t audio_ms;
  uint64_t start_ms;
  uint64_t end_ms;
  uint64_t ave_ms;

  uint32_t s_cnt;
};

/**
 * 全局维护一个服务鉴权token和其对应的有效期时间戳，
 * 每次调用服务之前，首先判断token是否已经过期，
 * 如果已经过期，则根据AccessKey ID和AccessKey Secret重新生成一个token，
 * 并更新这个全局的token和其有效期时间戳。
 *
 * 注意：不要每次调用服务之前都重新生成新token，
 * 只需在token即将过期时重新生成即可。所有的服务并发可共用一个token。
 */
std::string g_appkey = "";
std::string g_akId = "";
std::string g_akSecret = "";
std::string g_token = "";
std::string g_domain = "";
std::string g_api_version = "";
std::string g_url = "";
std::string g_audio_path = "";
int g_threads = 1;
int g_cpu = 1;
static int loop_timeout = LOOP_TIMEOUT; /*循环运行的时间, 单位s*/
static int loop_count = 0; /*循环测试某音频文件的次数, 设置后loop_timeout无效*/

long g_expireTime = -1;
volatile static bool global_run = false;
static pthread_mutex_t params_mtx; /*全局统计参数g_statistics的操作锁*/
static std::map<unsigned long, struct ParamStatistics *> g_statistics;
static int sample_rate = SAMPLE_RATE_16K;
static int frame_size = FRAME_16K_20MS; /*每次推送音频字节数.*/
static int encoder_type = ENCODER_OPUS;
static int logLevel = AlibabaNls::LogDebug; /* 0:为关闭log */
static int max_sentence_silence = 0; /*最大静音断句时间, 单位ms. 默认不设置.*/
static int run_cnt = 0;
static int run_start_failed = 0;
static int run_cancel = 0;
static int run_success = 0;
static int run_fail = 0;

static bool global_sys = true;
static PROFILE_INFO g_ave_percent;
static PROFILE_INFO g_min_percent;
static PROFILE_INFO g_max_percent;

static int profile_scan = -1;
static int cur_profile_scan = -1;
static PROFILE_INFO * g_sys_info = NULL;
static bool longConnection = false;
static bool sysAddrinfo = false;
static bool noSleepFlag = false;

void signal_handler_int(int signo) {
  std::cout << "\nget interrupt mesg\n" << std::endl;
  global_run = false;
}
void signal_handler_quit(int signo) {
  std::cout << "\nget quit mesg\n" << std::endl;
  global_run = false;
}

std::string timestamp_str() {
  char buf[64];
  struct timeval tv;
  struct tm ltm;

  gettimeofday(&tv, NULL);
  localtime_r(&tv.tv_sec, &ltm);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
           ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday,
           ltm.tm_hour, ltm.tm_min, ltm.tm_sec,
           tv.tv_usec);
  buf[63] = '\0';
  std::string tmp = buf;
  return tmp;
}

static void vectorStartStore(unsigned long pid) {
  pthread_mutex_lock(&params_mtx);

  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 已经存在
    struct timeval start_tv;
    gettimeofday(&start_tv, NULL);
    iter->second->start_ms = start_tv.tv_sec * 1000 + start_tv.tv_usec / 1000;
    std::cout << "vectorStartStore start:" << iter->second->start_ms << std::endl;
  }

  pthread_mutex_unlock(&params_mtx);
  return;
}

static void vectorSetParams(unsigned long pid, bool add,
                            struct ParamStatistics params) {
  pthread_mutex_lock(&params_mtx);

  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 已经存在
    iter->second->running = params.running;
    iter->second->success_flag = params.success_flag;
    iter->second->failed_flag = false;
    if (params.audio_ms > 0) {
      iter->second->audio_ms = params.audio_ms;
    }
  } else {
    // 不存在, 新的pid
    if (add) {
//      std::cout << "vectorSetParams create pid:" << pid << std::endl;
      struct ParamStatistics *p_tmp = new(struct ParamStatistics);
      if (!p_tmp) return;
      memset(p_tmp, 0, sizeof(struct ParamStatistics));
      p_tmp->running = params.running;
      p_tmp->success_flag = params.success_flag;
      p_tmp->failed_flag = false;
      if (params.audio_ms > 0) {
        p_tmp->audio_ms = params.audio_ms;
      }
      g_statistics.insert(std::make_pair(pid, p_tmp));
    } else {
    }
  }

  pthread_mutex_unlock(&params_mtx);
  return;
}

static void vectorSetRunning(unsigned long pid, bool run) {
  pthread_mutex_lock(&params_mtx);

  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
//  std::cout << "vectorSetRunning pid:"<< pid
//    << "; run:" << run << std::endl;
  if (iter != g_statistics.end()) {
    // 已经存在
    iter->second->running = run;
  } else {
  }

  pthread_mutex_unlock(&params_mtx);
  return;
}

static void vectorSetResult(unsigned long pid, bool ret) {
  pthread_mutex_lock(&params_mtx);

  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 已经存在
    iter->second->success_flag = ret;

    if (ret) {
      struct timeval end_tv;
      gettimeofday(&end_tv, NULL);
      iter->second->end_ms = end_tv.tv_sec * 1000 + end_tv.tv_usec / 1000;
      uint64_t d_ms = iter->second->end_ms - iter->second->start_ms;

      if (iter->second->ave_ms == 0) {
        iter->second->ave_ms = d_ms;
      } else {
        iter->second->ave_ms = (d_ms + iter->second->ave_ms) / 2;
      }
      iter->second->s_cnt++;
    }
  } else {
  }

  pthread_mutex_unlock(&params_mtx);
  return;
}

static void vectorSetFailed(unsigned long pid, bool ret) {
  pthread_mutex_lock(&params_mtx);

  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 已经存在
    iter->second->failed_flag = ret;
  } else {
  }

  pthread_mutex_unlock(&params_mtx);
  return;
}

static bool vectorGetRunning(unsigned long pid) {
  pthread_mutex_lock(&params_mtx);

  bool result = false;
  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 存在
    result = iter->second->running;
  } else {
    // 不存在, 新的pid
  }

  pthread_mutex_unlock(&params_mtx);
  return result;
}

static bool vectorGetFailed(unsigned long pid) {
  pthread_mutex_lock(&params_mtx);

  bool result = false;
  std::map<unsigned long, struct ParamStatistics*>::iterator iter;
  iter = g_statistics.find(pid);
  if (iter != g_statistics.end()) {
    // 存在
    result = iter->second->failed_flag;
  } else {
    // 不存在, 新的pid
  }

  pthread_mutex_unlock(&params_mtx);
  return result;
}

/**
 * 根据AccessKey ID和AccessKey Secret重新生成一个token，
 * 并获取其有效期时间戳
 */
int generateToken(std::string akId, std::string akSecret,
                  std::string* token, long* expireTime) {
  AlibabaNlsCommon::NlsToken nlsTokenRequest;
  nlsTokenRequest.setAccessKeyId(akId);
  nlsTokenRequest.setKeySecret(akSecret);
  if (!g_domain.empty()) {
    nlsTokenRequest.setDomain(g_domain);
  }
  if (!g_api_version.empty()) {
    nlsTokenRequest.setServerVersion(g_api_version);
  }

  int retCode = nlsTokenRequest.applyNlsToken();
  /*获取失败原因*/
  if (retCode < 0) {
    std::cout << "Failed error code: "
              << retCode
              << "  error msg: "
              << nlsTokenRequest.getErrorMsg()
              << std::endl;
    return retCode;
  }

  *token = nlsTokenRequest.getToken();
  *expireTime = nlsTokenRequest.getExpireTime();

  return 0;
}

unsigned int getAudioFileTimeMs(const int dataSize,
                                const int sampleRate,
                                const int compressRate) {
  // 仅支持16位采样
  const int sampleBytes = 16;
  // 仅支持单通道
  const int soundChannel = 1;

  // 当前采样率，采样位数下每秒采样数据的大小
  int bytes = (sampleRate * sampleBytes * soundChannel) / 8;

  // 当前采样率，采样位数下每毫秒采样数据的大小
  int bytesMs = bytes / 1000;

  // 待发送数据大小除以每毫秒采样数据大小，以获取sleep时间
  int fileMs = (dataSize * compressRate) / bytesMs;

  return fileMs;
}

/**
 * @brief 获取sendAudio发送延时时间
 * @param dataSize 待发送数据大小
 * @param sampleRate 采样率 16k/8K
 * @param compressRate 数据压缩率，例如压缩比为10:1的16k opus编码，此时为10；
 *                     非压缩数据则为1
 * @return 返回sendAudio之后需要sleep的时间
 * @note 对于8k pcm 编码数据, 16位采样，建议每发送1600字节 sleep 100 ms.
         对于16k pcm 编码数据, 16位采样，建议每发送3200字节 sleep 100 ms.
         对于其它编码格式(OPUS)的数据, 由于传递给SDK的仍然是PCM编码数据,
         按照SDK OPUS/OPU 数据长度限制, 需要每次发送640字节 sleep 20ms.
 */
unsigned int getSendAudioSleepTime(const int dataSize,
                                   const int sampleRate,
                                   const int compressRate) {
  int sleepMs = getAudioFileTimeMs(dataSize, sampleRate, compressRate);
  //std::cout << "data size: " << dataSize << "bytes, sleep: " << sleepMs << "ms." << std::endl;
  return sleepMs;
}

/**
 * @brief 调用start(), 成功与云端建立连接, sdk内部线程上报started事件
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void onTranscriptionStarted(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  std::cout << "onTranscriptionStarted:"
            << "  status code: " << cbEvent->getStatusCode()
            << "  task id: " << cbEvent->getTaskId()
            << "  onTranscriptionStarted: All response:"
            << cbEvent->getAllResponse()
            << std::endl;

  if (cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    if (!tmpParam->tParam) return;
    std::cout << "  onTranscriptionStarted Max Time: "
              << tmpParam->tParam->startMaxValue
              << "  userId: " << tmpParam->userId
              << std::endl;

    gettimeofday(&(tmpParam->startedTv), NULL);

    tmpParam->tParam->startedConsumed ++;

    unsigned long long timeValue1 =
        tmpParam->startedTv.tv_sec - tmpParam->startTv.tv_sec;
    unsigned long long timeValue2 =
        tmpParam->startedTv.tv_usec - tmpParam->startTv.tv_usec;
    unsigned long long timeValue = 0;
    if (timeValue1 > 0) {
      timeValue = (((timeValue1 * 1000000) + timeValue2) / 1000);
    } else {
      timeValue = (timeValue2 / 1000);
    }

    // max
    if (timeValue > tmpParam->tParam->startMaxValue) {
      tmpParam->tParam->startMaxValue = timeValue;
    }

    unsigned long long tmp = timeValue;
    if (tmp <= 50) {
      tmpParam->tParam->s50Value ++;
    } else if (tmp <= 100) {
      tmpParam->tParam->s100Value ++;
    } else if (tmp <= 200) {
      tmpParam->tParam->s200Value ++;
    } else if (tmp <= 500) {
      tmpParam->tParam->s500Value ++;
    } else if (tmp <= 1000) {
      tmpParam->tParam->s1000Value ++;
    } else {
      tmpParam->tParam->s2000Value ++;
    }

    // min
    if (tmpParam->tParam->startMinValue == 0) {
      tmpParam->tParam->startMinValue = timeValue;
    } else {
      if (timeValue < tmpParam->tParam->startMinValue) {
        tmpParam->tParam->startMinValue = timeValue;
      }
    }

    // ave
    tmpParam->tParam->startTotalValue += timeValue;
    if (tmpParam->tParam->startedConsumed > 0) {
      tmpParam->tParam->startAveValue =
          tmpParam->tParam->startTotalValue / tmpParam->tParam->startedConsumed;
    }

    // first package flag init
    tmpParam->tParam->firstFlag = false;

    // pid, add, run, success
    struct ParamStatistics params;
    params.running = true;
    params.success_flag = false;
    params.audio_ms = 0;
    vectorSetParams(tmpParam->userId, true, params);

    // 通知发送线程start()成功, 可以继续发送数据
    pthread_mutex_lock(&(tmpParam->mtxWord));
    pthread_cond_signal(&(tmpParam->cvWord));
    pthread_mutex_unlock(&(tmpParam->mtxWord));
  }
}

/**
 * @brief 服务端检测到了一句话的开始, sdk内部线程上报SentenceBegin事件
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
*/
void onSentenceBegin(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
#if 1
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  std::cout << "onSentenceBegin CbParam: " << tmpParam->userId
      << ", " << tmpParam->userInfo
      << std::endl; // 仅表示自定义参数示例
  std::cout << "  onSentenceBegin: "
      << "status code: " << cbEvent->getStatusCode() // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: " << cbEvent->getTaskId()   // 当前任务的task id，方便定位问题，建议输出
      << ", index: " << cbEvent->getSentenceIndex() //句子编号，从1开始递增
      << ", time: " << cbEvent->getSentenceTime() //当前已处理的音频时长，单位是毫秒
      << std::endl;

  std::cout << "  onSentenceBegin: All response:"
      << cbEvent->getAllResponse() << std::endl; // 获取服务端返回的全部信息
#endif
}

/**
 * @brief 服务端检测到了一句话结束, sdk内部线程上报SentenceEnd事件
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
*/
void onSentenceEnd(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
#if 1
  ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
  std::cout << "onSentenceEnd CbParam: " << tmpParam->userId
      << ", " << tmpParam->userInfo << std::endl; // 仅表示自定义参数示例

  std::cout << "  onSentenceEnd: "
      << "status code: " << cbEvent->getStatusCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: " << cbEvent->getTaskId()   // 当前任务的task id，方便定位问题，建议输出
      << ", result: " << cbEvent->getResult()    // 当前句子的完成识别结果
      << ", index: " << cbEvent->getSentenceIndex()  // 当前句子的索引编号
      << ", begin_time: " << cbEvent->getSentenceBeginTime() // 对应的SentenceBegin事件的时间
      << ", time: " << cbEvent->getSentenceTime()    // 当前句子的音频时长
      << ", confidence: " << cbEvent->getSentenceConfidence()    // 结果置信度,取值范围[0.0,1.0]，值越大表示置信度越高
      << ", stashResult begin_time: " << cbEvent->getStashResultBeginTime() //下一句话开始时间
      << ", stashResult current_time: " << cbEvent->getStashResultCurrentTime() //下一句话当前时间
      << ", stashResult Sentence_id: " << cbEvent->getStashResultSentenceId() //sentence Id
      << std::endl;

  /* 这里的start_time表示调用start后开始sendAudio传递Abytes音频时
   * 发现这句话的起点. 即调用start后传递start_time出现这句话的起点.
   * 这里的end_time表示调用start后开始sendAudio传递Bbytes音频时
   * 发现这句话的结尾. 即调用start后传递end_time出现这句话的结尾.
   */
  std::cout << "  onSentenceEnd: All response:"
            << cbEvent->getAllResponse()
            << std::endl; // 获取服务端返回的全部信息

  struct SentenceParamStruct param;
  param.sentenceId = cbEvent->getSentenceIndex();
  param.text.assign(cbEvent->getResult());
  param.beginTime = cbEvent->getSentenceBeginTime();
  param.endTime = cbEvent->getSentenceTime();
  gettimeofday(&(param.endTv), NULL);
  tmpParam->sentenceParam.push_back(param);
#endif
}

/**
 * @brief 识别结果发生了变化, sdk在接收到云端返回到最新结果时,
 *        sdk内部线程上报ResultChanged事件
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
*/
void onTranscriptionResultChanged(
    AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  if (cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    std::cout << "onTranscriptionResultChanged userId: " << tmpParam->userId
      << ", " << tmpParam->userInfo << std::endl; // 仅表示自定义参数示例

    if (tmpParam->tParam->firstFlag == false) {
      tmpParam->tParam->firstConsumed++;
      tmpParam->tParam->firstFlag = true;

      gettimeofday(&(tmpParam->firstTv), NULL);

      unsigned long long timeValue1 =
          tmpParam->firstTv.tv_sec - tmpParam->startTv.tv_sec;
      unsigned long long timeValue2 =
          tmpParam->firstTv.tv_usec - tmpParam->startTv.tv_usec;
      unsigned long long timeValue = 0;
      if (timeValue1 > 0) {
        timeValue = (((timeValue1 * 1000000) + timeValue2) / 1000);
      } else {
        timeValue = (timeValue2 / 1000);
      }

      // max
      if (timeValue > tmpParam->tParam->firstMaxValue) {
        tmpParam->tParam->firstMaxValue = timeValue;
      }
      // min
      if (tmpParam->tParam->firstMinValue == 0) {
        tmpParam->tParam->firstMinValue = timeValue;
      } else {
        if (timeValue < tmpParam->tParam->firstMinValue) {
          tmpParam->tParam->firstMinValue = timeValue;
        }
      }
      // ave
      tmpParam->tParam->firstTotalValue += timeValue;
      if (tmpParam->tParam->firstConsumed > 0) {
        tmpParam->tParam->firstAveValue =
            tmpParam->tParam->firstTotalValue / tmpParam->tParam->firstConsumed;
      }
    } // firstFlag
  }

  std::cout << "  onTranscriptionResultChanged: "
      << "status code: " << cbEvent->getStatusCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
      << ", task id: " << cbEvent->getTaskId()   // 当前任务的task id，方便定位问题，建议输出
      << ", result: " << cbEvent->getResult()    // 当前句子的中间识别结果
      << ", index: " << cbEvent->getSentenceIndex()  // 当前句子的索引编号
      << ", time: " << cbEvent->getSentenceTime()    // 当前句子的音频时长
      << std::endl;
  // std::cout << "onTranscriptionResultChanged: All response:"
  //     << cbEvent->getAllResponse() << std::endl; // 获取服务端返回的全部信息
}

/**
 * @brief 服务端停止实时音频流识别时, sdk内部线程上报Completed事件
 * @note 上报Completed事件之后，SDK内部会关闭识别连接通道. 
         此时调用sendAudio会返回负值, 请停止发送.
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
*/
void onTranscriptionCompleted(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  run_success++;

  std::cout << "onTranscriptionCompleted: "
    << " task id: " << cbEvent->getTaskId() << ", "
    << "status code: " << cbEvent->getStatusCode()  // 获取消息的状态码，成功为0或者20000000，失败时对应失败的错误码
    << std::endl;
  std::cout << "  onTranscriptionCompleted: All response:"
    << cbEvent->getAllResponse() << std::endl; // 获取服务端返回的全部信息

  if (cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    if (!tmpParam->tParam) return;

    std::cout << "  onTranscriptionCompleted Max Time: "
      << tmpParam->tParam->endMaxValue
      << " userId: " << tmpParam->userId
      << std::endl;

    gettimeofday(&(tmpParam->completedTv), NULL);
    tmpParam->tParam->completedConsumed ++;

    unsigned long long timeValue1 =
      tmpParam->completedTv.tv_sec - tmpParam->startTv.tv_sec;
    unsigned long long timeValue2 =
      tmpParam->completedTv.tv_usec - tmpParam->startTv.tv_usec;
    unsigned long long timeValue = 0;
    if (timeValue1 > 0) {
      timeValue = (((timeValue1 * 1000000) + timeValue2) / 1000);
    } else {
      timeValue = (timeValue2 / 1000);
    }

    // max
    if (timeValue > tmpParam->tParam->endMaxValue) {
      tmpParam->tParam->endMaxValue = timeValue;
    }
    // min
    if (tmpParam->tParam->endMinValue == 0) {
      tmpParam->tParam->endMinValue = timeValue;
    } else {
      if (timeValue < tmpParam->tParam->endMinValue) {
        tmpParam->tParam->endMinValue = timeValue;
      }
    }
    // ave
    tmpParam->tParam->endTotalValue += timeValue;
    if (tmpParam->tParam->completedConsumed > 0) {
      tmpParam->tParam->endAveValue =
          tmpParam->tParam->endTotalValue / tmpParam->tParam->completedConsumed;
    }

    vectorSetResult(tmpParam->userId, true);
  }
}

/**
 * @brief 识别过程(包含start(), sendAudio(), stop())发生异常时, sdk内部线程上报TaskFailed事件
 * @note 上报TaskFailed事件之后, SDK内部会关闭识别连接通道. 此时调用sendAudio会返回负值, 请停止发送
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void onTaskFailed(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  run_fail++;

  FILE *failed_stream = fopen("transcriptionTaskFailed.log", "a+");
  if (failed_stream) {
    std::string ts = timestamp_str();
    char outbuf[1024] = {0};
    snprintf(outbuf, sizeof(outbuf),
        "%s status code:%d task id:%s error mesg:%s\n",
        ts.c_str(),
        cbEvent->getStatusCode(),
        cbEvent->getTaskId(),
        cbEvent->getErrorMessage()
        );
    fwrite(outbuf, strlen(outbuf), 1, failed_stream);
    fclose(failed_stream);
  }

  std::cout << "onTaskFailed: "
      << "status code: " << cbEvent->getStatusCode()
      << ", task id: " << cbEvent->getTaskId()   // 当前任务的task id，方便定位问题，建议输出
      << ", error message: " << cbEvent->getErrorMessage()
      << std::endl;
  std::cout << "onTaskFailed: All response:"
      << cbEvent->getAllResponse() << std::endl; // 获取服务端返回的全部信息

  if (cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    if (!tmpParam->tParam) return;

    tmpParam->tParam->failedConsumed ++;

    std::cout << "  onTaskFailed userId " << tmpParam->userId
        << ", " << tmpParam->userInfo << std::endl; // 仅表示自定义参数示例

    vectorSetResult(tmpParam->userId, false);
    vectorSetFailed(tmpParam->userId, true);
  }
}

/**
 * @brief 服务端返回的所有信息会通过此回调反馈
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
*/
void onMessage(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  std::cout << "onMessage: All response:"
      << cbEvent->getAllResponse() << std::endl;
  std::cout << "onMessage: msg tyep:"
      << cbEvent->getMsgType() << std::endl;

  // 这里需要解析json
  int result = cbEvent->parseJsonMsg(true);
  if (result) {
    std::cout << "onMessage: parseJsonMsg failed:"
      << result << std::endl;
  } else {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    switch (cbEvent->getMsgType()) {
      case AlibabaNls::NlsEvent::TaskFailed:
        break;
      case AlibabaNls::NlsEvent::TranscriptionStarted:
        // 通知发送线程start()成功, 可以继续发送数据
        pthread_mutex_lock(&(tmpParam->mtxWord));
        pthread_cond_signal(&(tmpParam->cvWord));
        pthread_mutex_unlock(&(tmpParam->mtxWord));
        break;
      case AlibabaNls::NlsEvent::Close:
        //通知发送线程, 最终识别结果已经返回, 可以调用stop()
        pthread_mutex_lock(&(tmpParam->mtxWord));
        pthread_cond_signal(&(tmpParam->cvWord));
        pthread_mutex_unlock(&(tmpParam->mtxWord));
        break;
    }
  }
}

/**
 * @brief 识别结束或发生异常时，会关闭连接通道, sdk内部线程上报ChannelCloseed事件
 * @param cbEvent 回调事件结构, 详见nlsEvent.h
 * @param cbParam 回调自定义参数，默认为NULL, 可以根据需求自定义参数
 * @return
 */
void onChannelClosed(AlibabaNls::NlsEvent* cbEvent, void* cbParam) {
  std::cout << "OnChannelCloseed: All response: " << cbEvent->getAllResponse()
      << std::endl; // getResponse() 可以通道关闭信息

  if (cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    if (!tmpParam->tParam) {
      std::cout << "  OnChannelCloseed tParam is nullptr" << std::endl;
      return;
    }

    tmpParam->tParam->closeConsumed ++;
    gettimeofday(&(tmpParam->closedTv), NULL);

    unsigned long long timeValue1 =
        tmpParam->closedTv.tv_sec - tmpParam->startTv.tv_sec;
    unsigned long long timeValue2 =
        tmpParam->closedTv.tv_usec - tmpParam->startTv.tv_usec;
    unsigned long long timeValue = 0;
    if (timeValue1 > 0) {
      timeValue = (((timeValue1 * 1000000) + timeValue2) / 1000);
    } else {
      timeValue = (timeValue2 / 1000);
    }

    //max
    if (timeValue > tmpParam->tParam->closeMaxValue) {
      tmpParam->tParam->closeMaxValue = timeValue;
    }
    //min
    if (tmpParam->tParam->closeMinValue == 0) {
      tmpParam->tParam->closeMinValue = timeValue;
    } else {
      if (timeValue < tmpParam->tParam->closeMinValue) {
        tmpParam->tParam->closeMinValue = timeValue;
      }
    }
    //ave
    tmpParam->tParam->closeTotalValue += timeValue;
    if (tmpParam->tParam->closeConsumed > 0) {
      tmpParam->tParam->closeAveValue =
          tmpParam->tParam->closeTotalValue / tmpParam->tParam->closeConsumed;
    }

    std::cout << "  OnChannelCloseed: userId " << tmpParam->userId << ", "
      << tmpParam->userInfo << std::endl; // 仅表示自定义参数示例

    int vec_len = tmpParam->sentenceParam.size();
    if (vec_len > 0) {
      std::cout << "  \n=================================" << std::endl;
      std::cout << "  |  max sentence silence: " << max_sentence_silence << "ms" << std::endl;
      std::cout << "  |  frame size: " << frame_size << "bytes" << std::endl;
      std::cout << "  --------------------------------" << std::endl;
      unsigned long long timeValue0 =
          tmpParam->startTv.tv_sec * 1000 + tmpParam->startTv.tv_usec / 1000;
      std::cout << "  |  start tv: " << timeValue0 << "ms" << std::endl;

      unsigned long long timeValue1 =
          tmpParam->startedTv.tv_sec * 1000 + tmpParam->startedTv.tv_usec / 1000;
      std::cout << "  |  started tv: " << timeValue1 << "ms" << std::endl;
      std::cout << "  |    started duration: " << timeValue1 - timeValue0 << "ms" << std::endl;

      unsigned long long timeValue2 =
          tmpParam->startAudioTv.tv_sec * 1000 + tmpParam->startAudioTv.tv_usec / 1000;
      std::cout << "  |  start audio tv: " << timeValue2 << "ms" << std::endl;
      std::cout << "  |    start audio duration: " << timeValue2 - timeValue0 << "ms" << std::endl;
      std::cout << "  --------------------------------" << std::endl;
      for (int i = 0; i < vec_len; i++) {
        struct SentenceParamStruct tmp = tmpParam->sentenceParam[i];
        std::cout << "  |  index: " << tmp.sentenceId << std::endl;
        std::cout << "  |  sentence duration: " << tmp.beginTime
            << " - " << tmp.endTime
            << "ms = " << (tmp.endTime - tmp.beginTime) << "ms" << std:: endl;
        unsigned long long endTimeValue =
            tmp.endTv.tv_sec * 1000 + tmp.endTv.tv_usec / 1000;
        std::cout << "  |  end tv duration: " << timeValue2
            << " - " << endTimeValue
            << "ms = " << (endTimeValue - timeValue2) << "ms" << std:: endl;
        std::cout << "  |  text: " << tmp.text << std::endl;
        std::cout << "  --------------------------------" << std::endl;
      }
      std::cout << "  =================================\n" << std::endl;

      for (int j = 0; j < vec_len; j++) {
        tmpParam->sentenceParam.pop_back();
      }
      tmpParam->sentenceParam.clear();
    }

    //通知发送线程, 最终识别结果已经返回, 可以调用stop()
    pthread_mutex_lock(&(tmpParam->mtxWord));
    pthread_cond_signal(&(tmpParam->cvWord));
    pthread_mutex_unlock(&(tmpParam->mtxWord));
  }
}

void* autoCloseFunc(void* arg) {
  int timeout = 50;

  while (!global_run && timeout-- > 0) {
    usleep(100 * 1000);
  }
  timeout = loop_timeout;
  while (timeout-- > 0 && global_run) {
    usleep(1000 * 1000);

    if (g_sys_info) {
      int cur = -1;
      if (cur_profile_scan == -1) {
        cur = 0;
      } else if (cur_profile_scan == 0) {
        continue;
      } else {
        cur = cur_profile_scan;
      }
      PROFILE_INFO cur_sys_info;
      get_profile_info("stDemo", &cur_sys_info);
      std::cout << cur << ": cur_usr_name: " << cur_sys_info.usr_name
        << " CPU: " << cur_sys_info.ave_cpu_percent << "%"
        << " MEM: " << cur_sys_info.ave_mem_percent << "%"
        << std::endl;

      PROFILE_INFO *cur_info = &(g_sys_info[cur]);
      if (cur_info->ave_cpu_percent == 0) {
        strcpy(cur_info->usr_name, cur_sys_info.usr_name);
        cur_info->ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        cur_info->ave_mem_percent = cur_sys_info.ave_mem_percent;
        cur_info->eAveTime = 0;
      } else {
        if (cur_info->ave_cpu_percent < cur_sys_info.ave_cpu_percent) {
          cur_info->ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        }
        if (cur_info->ave_mem_percent < cur_sys_info.ave_mem_percent) {
          cur_info->ave_mem_percent = cur_sys_info.ave_mem_percent;
        }
      }
    }

    if (global_sys) {
      PROFILE_INFO cur_sys_info;
      get_profile_info("stDemo", &cur_sys_info);

      if (g_ave_percent.ave_cpu_percent == 0) {
        strcpy(g_ave_percent.usr_name, cur_sys_info.usr_name);
        strcpy(g_min_percent.usr_name, cur_sys_info.usr_name);
        strcpy(g_max_percent.usr_name, cur_sys_info.usr_name);

        g_ave_percent.ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        g_ave_percent.ave_mem_percent = cur_sys_info.ave_mem_percent;
        g_ave_percent.eAveTime = 0;

        g_min_percent.ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        g_min_percent.ave_mem_percent = cur_sys_info.ave_mem_percent;
        g_min_percent.eAveTime = 0;

        g_max_percent.ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        g_max_percent.ave_mem_percent = cur_sys_info.ave_mem_percent;
        g_max_percent.eAveTime = 0;
      } else {
        // record min info
        if (cur_sys_info.ave_cpu_percent < g_min_percent.ave_cpu_percent) {
          g_min_percent.ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        }
        if (cur_sys_info.ave_mem_percent < g_min_percent.ave_mem_percent) {
          g_min_percent.ave_mem_percent = cur_sys_info.ave_mem_percent;
        }
        // record max info
        if (cur_sys_info.ave_cpu_percent > g_max_percent.ave_cpu_percent) {
          g_max_percent.ave_cpu_percent = cur_sys_info.ave_cpu_percent;
        }
        if (cur_sys_info.ave_mem_percent > g_max_percent.ave_mem_percent) {
          g_max_percent.ave_mem_percent = cur_sys_info.ave_mem_percent;
        }
        // record ave info
        g_ave_percent.ave_cpu_percent =
            (g_ave_percent.ave_cpu_percent + cur_sys_info.ave_cpu_percent) / 2;
        g_ave_percent.ave_mem_percent =
            (g_ave_percent.ave_mem_percent + cur_sys_info.ave_mem_percent) / 2;
      }
    }

  }
  global_run = false;
  std::cout << "autoCloseFunc exit..." << pthread_self() << std::endl;
  return NULL;
}

/**
 * @brief 短链接模式下工作线程
 *        以 createTranscriberRequest           <----|
 *                   |                               |
 *           request->start()                        |
 *                   |                               |
 *           request->sendAudio()                    |
 *                   |                               |
 *           request->stop()                         |
 *                   |                               |
 *           收到onChannelClosed回调                 |
 *                   |                               |
 *           releaseTranscriberRequest(request)  ----|
 *        进行循环。
 */
void* pthreadFunction(void* arg) {
  int sleepMs = 0;
  int testCount = 0;
  uint64_t sendAudio_us = 0;
  uint32_t sendAudio_cnt = 0;
  bool timedwait_flag = false;

  ParamStruct* tst = (ParamStruct*)arg;
  if (tst == NULL) {
    std::cout << "arg is not valid." << std::endl;
    return NULL;
  }

  pthread_mutex_init(&(tst->mtx), NULL);

  /* 打开音频文件, 获取数据 */
  std::ifstream fs;
  fs.open(tst->fileName, std::ios::binary | std::ios::in);
  if (!fs) {
    std::cout << tst->fileName << " isn't exist.." << std::endl;
    return NULL;
  } else {
    fs.seekg(0, std::ios::end);
    int len = fs.tellg();
    tst->audioFileTimeLen = getAudioFileTimeMs(len, sample_rate, 1);

    struct ParamStatistics params;
    params.running = false;
    params.success_flag = false;
    params.audio_ms = len / 640 * 20;
    vectorSetParams(pthread_self(), true, params);
  }

  // 退出线程前释放
  ParamCallBack *cbParam = NULL;
  cbParam = new ParamCallBack(tst);
  if (!cbParam) {
    return NULL;
  }
  cbParam->userId = pthread_self();
  strcpy(cbParam->userInfo, "User.");

  do {
    //pthread_mutex_lock(&(cbParam->tParam->mtx));
    cbParam->tParam->requestConsumed ++;
    //pthread_mutex_unlock(&(cbParam->tParam->mtx));

    /*
     * 创建实时音频流识别SpeechTranscriberRequest对象
     */
    AlibabaNls::SpeechTranscriberRequest* request =
        AlibabaNls::NlsClient::getInstance()->createTranscriberRequest(
            "cpp", longConnection);
    if (request == NULL) {
      std::cout << "createTranscriberRequest failed." << std::endl;
      if (cbParam) {
        delete cbParam;
        cbParam = NULL;
      }
      return NULL;
    }

    // 设置识别启动回调函数
    request->setOnTranscriptionStarted(onTranscriptionStarted, cbParam);
    // 设置识别结果变化回调函数
    request->setOnTranscriptionResultChanged(
        onTranscriptionResultChanged, cbParam);
    // 设置语音转写结束回调函数
    request->setOnTranscriptionCompleted(onTranscriptionCompleted, cbParam);
    // 设置一句话开始回调函数
    request->setOnSentenceBegin(onSentenceBegin, cbParam);
    // 设置一句话结束回调函数
    request->setOnSentenceEnd(onSentenceEnd, cbParam);
    // 设置异常识别回调函数
    request->setOnTaskFailed(onTaskFailed, cbParam);
    // 设置识别通道关闭回调函数
    request->setOnChannelClosed(onChannelClosed, cbParam);
    // 设置所有服务端返回信息回调函数
    //request->setOnMessage(onMessage, cbParam);
    // 开启所有服务端返回信息回调函数, 其他回调(除了OnBinaryDataRecved)失效
    //request->setEnableOnMessage(true);

    // 设置AppKey, 必填参数, 请参照官网申请
    if (strlen(tst->appkey) > 0) {
      request->setAppKey(tst->appkey);
    }
    // 设置账号校验token, 必填参数
    if (strlen(tst->token) > 0) {
      request->setToken(tst->token);
    }
    if (strlen(tst->url) > 0) {
      request->setUrl(tst->url);
    }
    // 获取返回文本的编码格式
    const char* output_format = request->getOutputFormat();
    std::cout << "text format: " << output_format << std::endl;

    // 参数设置, 如指定声学模型
    //request->setPayloadParam("{\"model\":\"test-regression-model\"}");

    // 设置音频数据编码格式, 可选参数, 目前支持pcm,opus,opu. 默认是pcm
    if (encoder_type == ENCODER_OPUS) {
      request->setFormat("opus");
    } else if (encoder_type == ENCODER_OPU) {
      request->setFormat("opu");
    } else {
      request->setFormat("pcm");
    }
    // 设置音频数据采样率, 可选参数，目前支持16000, 8000. 默认是16000
    request->setSampleRate(sample_rate);
    // 设置是否返回中间识别结果, 可选参数. 默认false
    request->setIntermediateResult(true);
    // 设置是否在后处理中添加标点, 可选参数. 默认false
    request->setPunctuationPrediction(true);
    // 设置是否在后处理中执行数字转写, 可选参数. 默认false
    request->setInverseTextNormalization(true);

    // 语音断句检测阈值，一句话之后静音长度超过该值，即本句结束，合法参数范围200～2000(ms)，默认值800ms
    if (max_sentence_silence > 0) {
      if (max_sentence_silence > 2000 || max_sentence_silence < 200) {
        std::cout << "max sentence silence: " << max_sentence_silence
          << " is invalid" << std::endl;
      } else {
        request->setMaxSentenceSilence(max_sentence_silence);
      }
    }

    // 语义断句，启动此功能语音断句检测功能不会生效。此功能必须开启中间识别结果。
    //request->setSemanticSentenceDetection(true);

    //request->setCustomizationId("TestId_123"); //定制模型id, 可选.
    //request->setVocabularyId("TestId_456"); //定制泛热词id, 可选.

    // 设置链接超时时间
    //request->setTimeout(5000);
    // 设置发送超时时间
    //request->setSendTimeout(5000);
    // 设置是否开启接收超时
    //request->setEnableRecvTimeout(false);

    fs.clear();
    fs.seekg(0, std::ios::beg);

    gettimeofday(&(cbParam->startTv), NULL);
    int ret = request->start();
    run_cnt++;
    testCount++;
    if (ret < 0) {
      std::cout << "start() failed: " << ret << std::endl;
      run_start_failed++;
      // start()失败，释放request对象
      AlibabaNls::NlsClient::getInstance()->releaseTranscriberRequest(request);
      break;
    }

    // 等待started事件返回, 在发送
    std::cout << "wait started callback." << std::endl;
    /*
     * 语音服务器存在来不及处理当前请求, 10s内不返回任何回调的问题,
     * 然后在10s后返回一个TaskFailed回调, 所以需要设置一个超时机制.
     */
    struct timespec outtime;
    struct timeval now;
    gettimeofday(&now, NULL);
    outtime.tv_sec = now.tv_sec + OPERATION_TIMEOUT_S;
    outtime.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&(cbParam->mtxWord));
    if (ETIMEDOUT == pthread_cond_timedwait(&(cbParam->cvWord), &(cbParam->mtxWord), &outtime)) {
      std::cout << "start timeout." << std::endl;
      timedwait_flag = true;
      pthread_mutex_unlock(&(cbParam->mtxWord));
      request->cancel();
      run_cancel++;
      AlibabaNls::NlsClient::getInstance()->releaseTranscriberRequest(request);
      break;
    } else {
      std::cout << "start get started event." << std::endl;
    }
    pthread_mutex_unlock(&(cbParam->mtxWord));

    sendAudio_us = 0;
    sendAudio_cnt = 0;
    gettimeofday(&(cbParam->startAudioTv), NULL);
    while (!fs.eof()) {
      uint8_t data[frame_size];
      memset(data, 0, frame_size);

      fs.read((char *)data, sizeof(uint8_t) * frame_size);
      size_t nlen = fs.gcount();
      if (nlen <= 0) {
        std::cout << "fs empty..." << std::endl;
        continue;
      }

      struct timeval tv0, tv1;
      gettimeofday(&tv0, NULL);
      /*
       * 发送音频数据: sendAudio为异步操作, 返回负值表示发送失败,
       * 需要停止发送; 返回0 为成功.
       * notice : 返回值非成功发送字节数.
       * 若希望用省流量的opus格式上传音频数据, 则第三参数传入ENCODER_OPU
       * ENCODER_OPU/ENCODER_OPUS模式时, nlen必须为640
       */
      ret = request->sendAudio(data, nlen, (ENCODER_TYPE)encoder_type);
      //std::cout << "send audio nlen:" << nlen << ", ret:" << ret << std::endl;
      if (ret < 0) {
        // 发送失败, 退出循环数据发送
        std::cout << "send data fail(" << ret << ")." << std::endl;
        break;
      }

      /*
       * 运行过程中如果需要改参数, 可以调用control接口.
       * 以如下max_sentence_silence为例, 传入json字符串
       * 目前仅支持设置 max_sentence_silence和vocabulary_id
       */
      //request->control("{\"payload\":{\"max_sentence_silence\":2000}}");

      gettimeofday(&tv1, NULL);
      uint64_t tmp_us = (tv1.tv_sec - tv0.tv_sec) * 1000000 + tv1.tv_usec - tv0.tv_usec;
      sendAudio_us += tmp_us;
      sendAudio_cnt++;

      if (noSleepFlag) {
        /*
         * 不进行sleep, 用于测试性能.
         */
      } else {
        /*
         * 语音数据发送控制:
         * 语音数据是实时的, 不用sleep控制速率, 直接发送即可.
         * 语音数据来自文件, 发送时需要控制速率, 
         * 使单位时间内发送的数据大小接近单位时间原始语音数据存储的大小.
         */
        // 根据发送数据大小，采样率，数据压缩比 来获取sleep时间
        sleepMs = getSendAudioSleepTime(ret, sample_rate, 1);

        /*
         * 语音数据发送延时控制
         */
        if (sleepMs * 1000 > tmp_us) {
          usleep(sleepMs * 1000 - tmp_us);
        }
      }
    }  // while

    /*
    * 数据发送结束，关闭识别连接通道.
    * stop()为异步操作.
    */
    tst->sendConsumed += sendAudio_cnt;
    tst->sendTotalValue += sendAudio_us;
    if (sendAudio_cnt > 0) {
      std::cout << "sendAudio ave: " << (sendAudio_us / sendAudio_cnt) << "us" << std::endl;
    }
    std::cout << "stop ->" << std::endl;
    // stop()后会收到所有回调，若想立即停止则调用cancel()取消所有回调
    ret = request->stop();
    std::cout << "stop done. ret " << ret << "\n" << std::endl;

    /*
     * 识别结束, 释放request对象
     */
    if (ret == 0) {
      std::cout << "wait closed callback." << std::endl;
      /*
       * 语音服务器存在来不及处理当前请求, 10s内不返回任何回调的问题,
       * 然后在10s后返回一个TaskFailed回调, 错误信息为:
       * "Gateway:IDLE_TIMEOUT:Websocket session is idle for too long time, the last directive is 'StopTranscriber'!"
       * 所以需要设置一个超时机制.
       */
      gettimeofday(&now, NULL);
      outtime.tv_sec = now.tv_sec + OPERATION_TIMEOUT_S;
      outtime.tv_nsec = now.tv_usec * 1000;
      // 等待closed事件后再进行释放，否则会出现崩溃
      pthread_mutex_lock(&(cbParam->mtxWord));
      if (ETIMEDOUT == pthread_cond_timedwait(&(cbParam->cvWord), &(cbParam->mtxWord), &outtime)) {
        std::cout << "stop timeout" << std::endl;
        timedwait_flag = true;
        pthread_mutex_unlock(&(cbParam->mtxWord));
        AlibabaNls::NlsClient::getInstance()->releaseTranscriberRequest(request);
        break;
      }
      pthread_mutex_unlock(&(cbParam->mtxWord));
    } else {
      std::cout << "ret is " << ret << std::endl;
    }

    AlibabaNls::NlsClient::getInstance()->releaseTranscriberRequest(request);

    if (loop_count > 0 && testCount >= loop_count) {
      global_run = false;
    }
  } while (global_run);

  // 关闭音频文件
  fs.close();

  pthread_mutex_destroy(&(tst->mtx));

  if (timedwait_flag) {
    /*
     * stop超时的情况下, 会在10s后返回TaskFailed和Closed回调.
     * 若在回调前delete cbParam, 会导致回调中对cbParam的操作变成野指针操作，
     * 故若存在cbParam, 则在这里等一会
     */
    usleep(10 * 1000 * 1000);
  }

  if (cbParam) {
    delete cbParam;
    cbParam = NULL;
  }

  return NULL;
}

/**
 * @brief 长链接模式下工作线程
 *                  createTranscriberRequest
 *                          |
 *        然后以    request->start() <----------|
 *                          |                   |
 *                  request->sendAudio()        |
 *                          |                   |
 *                  request->stop()             |
 *                          |                   |
 *                  收到onChannelClosed回调  ---|
 *        进行循环          |
 *                  releaseTranscriberRequest(request)
 */
void* pthreadLongConnectionFunction(void* arg) {
  int sleepMs = 0;
  int testCount = 0;
  ParamCallBack *cbParam = NULL;
  uint64_t sendAudio_us = 0;
  uint32_t sendAudio_cnt = 0;
  bool timedwait_flag = false;

  ParamStruct* tst = (ParamStruct*)arg;
  if (tst == NULL) {
    std::cout << "arg is not valid." << std::endl;
    return NULL;
  }

  // 退出线程前释放
  cbParam = new ParamCallBack(tst);
  if (!cbParam) {
    return NULL;
  }
  cbParam->userId = pthread_self();
  strcpy(cbParam->userInfo, "User.");

  /*
   * 创建实时音频流识别SpeechTranscriberRequest对象
   */
  AlibabaNls::SpeechTranscriberRequest* request =
      AlibabaNls::NlsClient::getInstance()->createTranscriberRequest("cpp", longConnection);
  if (request == NULL) {
    std::cout << "createTranscriberRequest failed." << std::endl;
    delete cbParam;
    cbParam = NULL;
    return NULL;
  }

  // 设置识别启动回调函数
  request->setOnTranscriptionStarted(onTranscriptionStarted, cbParam);
  // 设置识别结果变化回调函数
  request->setOnTranscriptionResultChanged(onTranscriptionResultChanged, cbParam);
  // 设置语音转写结束回调函数
  request->setOnTranscriptionCompleted(onTranscriptionCompleted, cbParam);
  // 设置一句话开始回调函数
  request->setOnSentenceBegin(onSentenceBegin, cbParam);
  // 设置一句话结束回调函数
  request->setOnSentenceEnd(onSentenceEnd, cbParam);
  // 设置异常识别回调函数
  request->setOnTaskFailed(onTaskFailed, cbParam);
  // 设置识别通道关闭回调函数
  request->setOnChannelClosed(onChannelClosed, cbParam);
  // 设置所有服务端返回信息回调函数
  //request->setOnMessage(onMessage, cbParam);
  // 开启所有服务端返回信息回调函数, 其他回调(除了OnBinaryDataRecved)失效
  //request->setEnableOnMessage(true);

  // 设置AppKey, 必填参数, 请参照官网申请
  if (strlen(tst->appkey) > 0) {
    request->setAppKey(tst->appkey);
  }
  // 设置账号校验token, 必填参数
  if (strlen(tst->token) > 0) {
    request->setToken(tst->token);
  }
  if (strlen(tst->url) > 0) {
    request->setUrl(tst->url);
  }
  // 获取返回文本的编码格式
  const char* output_format = request->getOutputFormat();
  std::cout << "text format: " << output_format << std::endl;

  // 参数设置, 如指定声学模型
  //request->setPayloadParam("{\"model\":\"test-regression-model\"}");

  // 设置音频数据编码格式, 可选参数, 目前支持pcm,opus,opu. 默认是pcm
  if (encoder_type == ENCODER_OPUS) {
    request->setFormat("opus");
  } else if (encoder_type == ENCODER_OPU) {
    request->setFormat("opu");
  } else {
    request->setFormat("pcm");
  }
  // 设置音频数据采样率, 可选参数，目前支持16000, 8000. 默认是16000
  request->setSampleRate(sample_rate);
  // 设置是否返回中间识别结果, 可选参数. 默认false
  request->setIntermediateResult(true);
  // 设置是否在后处理中添加标点, 可选参数. 默认false
  request->setPunctuationPrediction(true);
  // 设置是否在后处理中执行数字转写, 可选参数. 默认false
  request->setInverseTextNormalization(true);

  // 语音断句检测阈值，一句话之后静音长度超过该值，即本句结束，
  // 合法参数范围200～2000(ms)，默认值800ms
  if (max_sentence_silence > 0) {
    if (max_sentence_silence > 2000 || max_sentence_silence < 200) {
      std::cout << "max sentence silence: " << max_sentence_silence
        << " is invalid" << std::endl;
    } else {
      request->setMaxSentenceSilence(max_sentence_silence);
    }
  }

  // 语义断句，启动此功能语音断句检测功能不会生效。此功能必须开启中间识别结果。
  //request->setSemanticSentenceDetection(true);

  //request->setCustomizationId("TestId_123"); //定制模型id, 可选.
  //request->setVocabularyId("TestId_456"); //定制泛热词id, 可选.

  pthread_mutex_init(&(tst->mtx), NULL);
  struct ParamStatistics params;

  do {
    /* 打开音频文件, 获取数据 */
    std::ifstream fs;
    fs.open(tst->fileName, std::ios::binary | std::ios::in);
    if (!fs) {
      std::cout << tst->fileName << " isn't exist.." << std::endl;
      break;
    } else {
      fs.seekg(0, std::ios::end);
      int len = fs.tellg();
      tst->audioFileTimeLen = getAudioFileTimeMs(len, sample_rate, 1);
      fs.seekg(0, std::ios::beg);

      params.running = false;
      params.success_flag = false;
      params.audio_ms = len / 640 * 20;
      vectorSetParams(pthread_self(), true, params);
    }

    cbParam->tParam->requestConsumed ++;

    gettimeofday(&(cbParam->startTv), NULL);
    int ret = request->start();
    run_cnt++;
    testCount++;
    if (ret < 0) {
      run_start_failed++;
      std::cout << "start() failed: " << ret << std::endl;
      break;
    }

    // 等待started事件返回, 在发送
    std::cout << "wait started callback." << std::endl;
    /*
     * 语音服务器存在来不及处理当前请求, 10s内不返回任何回调的问题,
     * 然后在10s后返回一个TaskFailed回调, 所以需要设置一个超时机制.
     */
    struct timespec outtime;
    struct timeval now;
    gettimeofday(&now, NULL);
    outtime.tv_sec = now.tv_sec + OPERATION_TIMEOUT_S;
    outtime.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&(cbParam->mtxWord));
    if (ETIMEDOUT == pthread_cond_timedwait(&(cbParam->cvWord), &(cbParam->mtxWord), &outtime)) {
      std::cout << "start timeout" << std::endl;
      timedwait_flag = true;
      pthread_mutex_unlock(&(cbParam->mtxWord));
      request->cancel();
      run_cancel++;
      break;
    }
    pthread_mutex_unlock(&(cbParam->mtxWord));

    sendAudio_us = 0;
    sendAudio_cnt = 0;
    gettimeofday(&(cbParam->startAudioTv), NULL);
    while (!fs.eof()) {
      uint8_t data[frame_size];
      memset(data, 0, frame_size);

      fs.read((char *)data, sizeof(uint8_t) * frame_size);
      size_t nlen = fs.gcount();
      if (nlen <= 0) {
        std::cout << "fs empty..." << std::endl;
        continue;
      }

      struct timeval tv0, tv1;
      gettimeofday(&tv0, NULL);
      /*
       * 发送音频数据: sendAudio为异步操作, 返回负值表示发送失败,
       * 需要停止发送; 返回0 为成功.
       * notice : 返回值非成功发送字节数.
       * 若希望用省流量的opus格式上传音频数据, 则第三参数传入ENCODER_OPU
       * ENCODER_OPU/ENCODER_OPUS模式时,nlen必须为640
       */
      ret = request->sendAudio(data, nlen, (ENCODER_TYPE)encoder_type);
      if (ret < 0) {
        // 发送失败, 退出循环数据发送
        std::cout << "send data fail(" << ret << ")." << std::endl;
        break;
      }
      gettimeofday(&tv1, NULL);
      uint64_t tmp_us =
          (tv1.tv_sec - tv0.tv_sec) * 1000000 + tv1.tv_usec - tv0.tv_usec;
      sendAudio_us += tmp_us;
      sendAudio_cnt++;

      if (noSleepFlag) {
        /*
         * 不进行sleep, 用于测试性能.
         */
      } else {
        /*
         * 语音数据发送控制：
         * 语音数据是实时的, 不用sleep控制速率, 直接发送即可.
         * 语音数据来自文件, 发送时需要控制速率,
         * 使单位时间内发送的数据大小接近单位时间原始语音数据存储的大小.
         */
        // 根据发送数据大小，采样率，数据压缩比 来获取sleep时间
        sleepMs = getSendAudioSleepTime(ret, sample_rate, 1);

        /*
         * 语音数据发送延时控制
         */
        if (sleepMs * 1000 > tmp_us) {
          usleep(sleepMs * 1000 - tmp_us);
        }
      }
    }  // while

    fs.clear();
    fs.close();

    /*
     * 数据发送结束，关闭识别连接通道.
     * stop()为异步操作.
     */
    tst->sendConsumed += sendAudio_cnt;
    tst->sendTotalValue += sendAudio_us;
    if (sendAudio_cnt > 0) {
      std::cout << "sendAudio ave: " << (sendAudio_us / sendAudio_cnt)
        << "us" << std::endl;
    }
    std::cout << "stop ->" << std::endl;
    // stop()后会收到所有回调，若想立即停止则调用cancel()取消所有回调
    ret = request->stop();
    std::cout << "stop done. ret " << ret << "\n" << std::endl;

    /*
     * 识别结束, 释放request对象
     */
    if (ret == 0) {
      std::cout << "wait closed callback." << std::endl;
      /*
       * 语音服务器存在来不及处理当前请求, 10s内不返回任何回调的问题,
       * 然后在10s后返回一个TaskFailed回调, 错误信息为:
       * "Gateway:IDLE_TIMEOUT:Websocket session is idle for too long time, the last directive is 'StopTranscriber'!"
       * 所以需要设置一个超时机制.
       */
      gettimeofday(&now, NULL);
      outtime.tv_sec = now.tv_sec + OPERATION_TIMEOUT_S;
      outtime.tv_nsec = now.tv_usec * 1000;
      // 等待closed事件后再进行释放，否则会出现崩溃
      pthread_mutex_lock(&(cbParam->mtxWord));
      if (ETIMEDOUT == pthread_cond_timedwait(&(cbParam->cvWord), &(cbParam->mtxWord), &outtime)) {
        std::cout << "stop timeout" << std::endl;
        pthread_mutex_unlock(&(cbParam->mtxWord));
        break;
      }
      pthread_mutex_unlock(&(cbParam->mtxWord));
    } else {
      std::cout << "ret is " << ret << std::endl;
    }

    if (loop_count > 0 && testCount >= loop_count) {
      global_run = false;
    }
  } while (global_run);

  pthread_mutex_destroy(&(tst->mtx));

  AlibabaNls::NlsClient::getInstance()->releaseTranscriberRequest(request);
  request = NULL;

  if (timedwait_flag) {
    /*
     * stop超时的情况下, 会在10s后返回TaskFailed和Closed回调.
     * 若在回调前delete cbParam, 会导致回调中对cbParam的操作变成野指针操作，
     * 故若存在cbParam, 则在这里等一会
     */
    usleep(10 * 1000 * 1000);
  }

  if (cbParam) {
    delete cbParam;
    cbParam = NULL;
  }

  return NULL;
}

/**
 * 识别多个音频数据;
 * sdk多线程指一个音频数据对应一个线程, 非一个音频数据对应多个线程.
 * 示例代码为同时开启threads个线程识别4个文件;
 * 免费用户并发连接不能超过10个;
 * notice: Linux高并发用户注意系统最大文件打开数限制, 详见README.md
 */
#define AUDIO_FILE_NUMS 4
#define AUDIO_FILE_NAME_LENGTH 32
int speechTranscriberMultFile(const char* appkey, int threads) {
  /**
   * 获取当前系统时间戳，判断token是否过期
   */
  std::time_t curTime = std::time(0);
  if (g_token.empty()) {
    if (g_expireTime - curTime < 10) {
      std::cout << "the token will be expired, please generate new token by AccessKey-ID and AccessKey-Secret." << std::endl;
      if (generateToken(g_akId, g_akSecret, &g_token, &g_expireTime) < 0) {
        return -1;
      }
    }
  }

#ifdef SELF_TESTING_TRIGGER
  if (loop_count == 0) {
    pthread_t p_id;
    pthread_create(&p_id, NULL, &autoCloseFunc, NULL);
    pthread_detach(p_id);
  }
#endif

  char audioFileNames[AUDIO_FILE_NUMS][AUDIO_FILE_NAME_LENGTH] =
  {
    "test0.wav", "test1.wav", "test2.wav", "test3.wav"
  };
  ParamStruct pa[threads];

  // init ParamStruct
  for (int i = 0; i < threads; i ++) {
    memset(pa[i].fileName, 0, DEFAULT_STRING_LEN);
    if (g_audio_path.empty()) {
      int num = i % AUDIO_FILE_NUMS;
      strncpy(pa[i].fileName, audioFileNames[num], strlen(audioFileNames[num]));
    } else {
      strncpy(pa[i].fileName, g_audio_path.c_str(), DEFAULT_STRING_LEN);
    }

    memset(pa[i].token, 0, DEFAULT_STRING_LEN);
    memcpy(pa[i].token, g_token.c_str(), g_token.length());

    memset(pa[i].appkey, 0, DEFAULT_STRING_LEN);
    memcpy(pa[i].appkey, appkey, strlen(appkey));

    memset(pa[i].url, 0, DEFAULT_STRING_LEN);
    if (!g_url.empty()) {
      memcpy(pa[i].url, g_url.c_str(), g_url.length());
    }

    pa[i].startedConsumed = 0;
    pa[i].firstConsumed = 0;
    pa[i].completedConsumed = 0;
    pa[i].closeConsumed = 0;
    pa[i].failedConsumed = 0;
    pa[i].requestConsumed = 0;
    pa[i].sendConsumed = 0;

    pa[i].startTotalValue = 0;
    pa[i].startAveValue = 0;
    pa[i].startMaxValue = 0;
    pa[i].startMinValue = 0;

    pa[i].firstTotalValue = 0;
    pa[i].firstAveValue = 0;
    pa[i].firstMaxValue = 0;
    pa[i].firstMinValue = 0;
    pa[i].firstFlag = false;

    pa[i].endTotalValue = 0;
    pa[i].endAveValue = 0;
    pa[i].endMaxValue = 0;
    pa[i].endMinValue = 0;

    pa[i].closeTotalValue = 0;
    pa[i].closeAveValue = 0;
    pa[i].closeMaxValue = 0;
    pa[i].closeMinValue = 0;
    pa[i].sendTotalValue = 0;

    pa[i].audioFileTimeLen = 0;

    pa[i].s50Value = 0;
    pa[i].s100Value = 0;
    pa[i].s200Value = 0;
    pa[i].s500Value = 0;
    pa[i].s1000Value = 0;
    pa[i].s2000Value = 0;
  }

  global_run = true;
  std::vector<pthread_t> pthreadId(threads);
  // 启动threads个工作线程, 同时识别threads个音频文件
  for (int j = 0; j < threads; j++) {
    if (longConnection) {
      pthread_create(&pthreadId[j], NULL, &pthreadLongConnectionFunction, (void *)&(pa[j]));
    } else {
      pthread_create(&pthreadId[j], NULL, &pthreadFunction, (void *)&(pa[j]));
    }
  }

  for (int j = 0; j < threads; j++) {
    pthread_join(pthreadId[j], NULL);
  }

  unsigned long long sTotalCount = 0; /*started总次数*/
  unsigned long long iTotalCount = 0; /*首包总次数*/
  unsigned long long eTotalCount = 0; /*completed总次数*/
  unsigned long long fTotalCount = 0; /*failed总次数*/
  unsigned long long cTotalCount = 0; /*closed总次数*/
  unsigned long long rTotalCount = 0; /*总请求数*/

  unsigned long long sMaxTime = 0;
  unsigned long long sMinTime = 0;
  unsigned long long sAveTime = 0;

  unsigned long long fMaxTime = 0; /*首包最大耗时*/
  unsigned long long fMinTime = 0; /*首包最小耗时*/
  unsigned long long fAveTime = 0; /*首包平均耗时*/

  unsigned long long s50Count = 0;
  unsigned long long s100Count = 0;
  unsigned long long s200Count = 0;
  unsigned long long s500Count = 0;
  unsigned long long s1000Count = 0;
  unsigned long long s2000Count = 0;

  unsigned long long eMaxTime = 0;
  unsigned long long eMinTime = 0;
  unsigned long long eAveTime = 0;

  unsigned long long cMaxTime = 0;
  unsigned long long cMinTime = 0;
  unsigned long long cAveTime = 0;

  unsigned long long sendTotalCount = 0;
  unsigned long long sendTotalTime = 0;
  unsigned long long sendAveTime = 0;

  unsigned long long audioFileAveTimeLen = 0;

  for (int i = 0; i < threads; i ++) {
    sTotalCount += pa[i].startedConsumed;
    iTotalCount += pa[i].firstConsumed;
    eTotalCount += pa[i].completedConsumed;
    fTotalCount += pa[i].failedConsumed;
    cTotalCount += pa[i].closeConsumed;
    rTotalCount += pa[i].requestConsumed;
    sendTotalCount += pa[i].sendConsumed;
    sendTotalTime += pa[i].sendTotalValue; // us, 所有线程sendAudio耗时总和
    audioFileAveTimeLen += pa[i].audioFileTimeLen;

    //std::cout << "Closed:" << pa[i].closeConsumed << std::endl;

    // start
    if (pa[i].startMaxValue > sMaxTime) {
      sMaxTime = pa[i].startMaxValue;
    }

    if (sMinTime == 0) {
      sMinTime = pa[i].startMinValue;
    } else {
      if (pa[i].startMinValue < sMinTime) {
        sMinTime = pa[i].startMinValue;
      }
    }

    sAveTime += pa[i].startAveValue;

    s50Count += pa[i].s50Value;
    s100Count += pa[i].s100Value;
    s200Count += pa[i].s200Value;
    s500Count += pa[i].s500Value;
    s1000Count += pa[i].s1000Value;
    s2000Count += pa[i].s2000Value;

    // first pack
    if (pa[i].firstMaxValue > fMaxTime) {
      fMaxTime = pa[i].firstMaxValue;
    }

    if (fMinTime == 0) {
      fMinTime = pa[i].firstMinValue;
    } else {
      if (pa[i].firstMinValue < fMinTime) {
        fMinTime = pa[i].firstMinValue;
      }
    }

    fAveTime += pa[i].firstAveValue;

    // end
    if (pa[i].endMaxValue > eMaxTime) {
      eMaxTime = pa[i].endMaxValue;
    }

    if (eMinTime == 0) {
      eMinTime = pa[i].endMinValue;
    } else {
      if (pa[i].endMinValue < eMinTime) {
        eMinTime = pa[i].endMinValue;
      }
    }

    eAveTime += pa[i].endAveValue;

    // close
    if (pa[i].closeMaxValue > cMaxTime) {
      cMaxTime = pa[i].closeMaxValue;
    }

    if (cMinTime == 0) {
      cMinTime = pa[i].closeMinValue;
    } else {
      if (pa[i].closeMinValue < cMinTime) {
        cMinTime = pa[i].closeMinValue;
      }
    }

    cAveTime += pa[i].closeAveValue;
  }

  sAveTime /= threads;
  eAveTime /= threads;
  cAveTime /= threads;
  fAveTime /= threads;
  audioFileAveTimeLen /= threads;

  int cur = -1;
  if (cur_profile_scan == -1) {
    cur = 0;
  } else if (cur_profile_scan == 0) {
  } else {
    cur = cur_profile_scan;
  }
  if (g_sys_info && cur >= 0 && cur_profile_scan != 0) {
    PROFILE_INFO *cur_info = &(g_sys_info[cur]);
    cur_info->eAveTime = eAveTime;
  }

  if (sendTotalCount > 0) {
    sendAveTime = sendTotalTime / sendTotalCount;
  }

  for (int i = 0; i < threads; i ++) {
    std::cout << "-----" << std::endl;
    std::cout << "No." << i
      << " Max started time: " << pa[i].startMaxValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Min started time: " << pa[i].startMinValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Ave started time: " << pa[i].startAveValue << " ms"
      << std::endl;

    std::cout << "No." << i
      << " Max first package time: " << pa[i].firstMaxValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Min first package time: " << pa[i].firstMinValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Ave first package time: " << pa[i].firstAveValue << " ms"
      << std::endl;

    std::cout << "No." << i
      << " Max completed time: " << pa[i].endMaxValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Min completed time: " << pa[i].endMinValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Ave completed time: " << pa[i].endAveValue << " ms"
      << std::endl;

    std::cout << "No." << i
      << " Max closed time: " << pa[i].closeMaxValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Min closed time: " << pa[i].closeMinValue << " ms"
      << std::endl;
    std::cout << "No." << i
      << " Ave closed time: " << pa[i].closeAveValue << " ms"
      << std::endl;

    std::cout << "No." << i
      << " Audio File duration: " << pa[i].audioFileTimeLen << " ms"
      << std::endl;
  }

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Final Total. " << std::endl;
  std::cout << "Final Request: " << rTotalCount << std::endl;
  std::cout << "Final Started: " << sTotalCount << std::endl;
  std::cout << "Final Completed: " << eTotalCount << std::endl;
  std::cout << "Final Failed: " << fTotalCount << std::endl;
  std::cout << "Final Closed: " << cTotalCount << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Max started time: " << sMaxTime << " ms" << std::endl;
  std::cout << "Min started time: " << sMinTime << " ms" << std::endl;
  std::cout << "Ave started time: " << sAveTime << " ms" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Started time <= 50 ms: " << s50Count << std::endl;
  std::cout << "Started time <= 100 ms: " << s100Count << std::endl;
  std::cout << "Started time <= 200 ms: " << s200Count << std::endl;
  std::cout << "Started time <= 500 ms: " << s500Count << std::endl;
  std::cout << "Started time <= 1000 ms: " << s1000Count << std::endl;
  std::cout << "Started time > 1000 ms: " << s2000Count << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Max first package time: " << fMaxTime << " ms" << std::endl;
  std::cout << "Min first package time: " << fMinTime << " ms" << std::endl;
  std::cout << "Ave first package time: " << fAveTime << " ms" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Final Max completed time: " << eMaxTime << " ms" << std::endl;
  std::cout << "Final Min completed time: " << eMinTime << " ms" << std::endl;
  std::cout << "Final Ave completed time: " << eAveTime << " ms" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Ave sendAudio time: " << sendAveTime << " us" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Max closed time: " << cMaxTime << " ms" << std::endl;
  std::cout << "Min closed time: " << cMinTime << " ms" << std::endl;
  std::cout << "Ave closed time: " << cAveTime << " ms" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "Ave audio file duration: " << audioFileAveTimeLen
            << " ms" << std::endl;

  std::cout << "\n ------------------- \n" << std::endl;

  std::cout << "speechTranscribeMultFile exit..." << std::endl;
  return 0;
}

int invalied_argv(int index, int argc) {
  if (index >= argc) {
    std::cout << "invalid params..." << std::endl;
    return 1;
  }
  return 0;
}

int parse_argv(int argc, char* argv[]) {
  int index = 1;
  while (index < argc) {
    if (!strcmp(argv[index], "--appkey")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_appkey = argv[index];
    } else if (!strcmp(argv[index], "--akId")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_akId = argv[index];
    } else if (!strcmp(argv[index], "--akSecret")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_akSecret = argv[index];
    } else if (!strcmp(argv[index], "--token")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_token = argv[index];
    } else if (!strcmp(argv[index], "--tokenDomain")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_domain = argv[index];
    } else if (!strcmp(argv[index], "--tokenApiVersion")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_api_version = argv[index];
    } else if (!strcmp(argv[index], "--url")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_url = argv[index];
    } else if (!strcmp(argv[index], "--threads")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_threads = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--cpu")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_cpu = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--time")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      loop_timeout = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--loop")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      loop_count = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--type")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      if (strcmp(argv[index], "pcm") == 0) {
        encoder_type = ENCODER_NONE;
        frame_size = FRAME_16K_100MS;
      } else if (strcmp(argv[index], "opu") == 0) {
        encoder_type = ENCODER_OPU;
        frame_size = FRAME_16K_20MS;
      } else if (strcmp(argv[index], "opus") == 0) {
        encoder_type = ENCODER_OPUS;
        frame_size = FRAME_16K_20MS;
      }
    } else if (!strcmp(argv[index], "--log")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      logLevel = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--sampleRate")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      sample_rate = atoi(argv[index]);
      if (sample_rate == SAMPLE_RATE_8K) {
        frame_size = FRAME_8K_20MS;
      } else if (sample_rate == SAMPLE_RATE_16K) {
        frame_size = FRAME_16K_20MS;
      }
    } else if (!strcmp(argv[index], "--frameSize")) {
      index++;
      frame_size = atoi(argv[index]);
      encoder_type = ENCODER_NONE;
    } else if (!strcmp(argv[index], "--NlsScan")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      profile_scan = atoi(argv[index]);
    } else if (!strcmp(argv[index], "--long")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      if (atoi(argv[index])) {
        longConnection = true;
      } else {
        longConnection = false;
      }
    } else if (!strcmp(argv[index], "--sys")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      if (atoi(argv[index])) {
        sysAddrinfo = true;
      } else {
        sysAddrinfo = false;
      }
    } else if (!strcmp(argv[index], "--noSleep")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      if (atoi(argv[index])) {
        noSleepFlag = true;
      } else {
        noSleepFlag = false;
      }
    } else if (!strcmp(argv[index], "--audioFile")) {
      index++;
      if (invalied_argv(index, argc)) return 1;
      g_audio_path = argv[index];
    } else if (!strcmp(argv[index], "--maxSilence")){
      index++;
      if (invalied_argv(index, argc)) return 1;
      max_sentence_silence = atoi(argv[index]);
    }
    index++;
  }
  if ((g_token.empty() && (g_akId.empty() || g_akSecret.empty())) ||
      g_appkey.empty()) {
    std::cout << "short of params..." << std::endl;
    return 1;
  }
  return 0;
}

int main(int argc, char* argv[]) {
  if (parse_argv(argc, argv)) {
    std::cout << "params is not valid.\n"
      << "Usage:\n"
      << "  --appkey <appkey>\n"
      << "  --akId <AccessKey ID>\n"
      << "  --akSecret <AccessKey Secret>\n"
      << "  --token <Token>\n"
      << "  --tokenDomain <the domain of token>\n"
      << "      mcos: mcos.cn-shanghai.aliyuncs.com\n"
      << "  --tokenApiVersion <the ApiVersion of token>\n"
      << "      mcos:  2022-08-11\n"
      << "  --url <Url>\n"
      << "      public(default): wss://nls-gateway.cn-shanghai.aliyuncs.com/ws/v1\n"
      << "      internal: ws://nls-gateway.cn-shanghai-internal.aliyuncs.com/ws/v1\n"
      << "      mcos: wss://mcos-cn-shanghai.aliyuncs.com/ws/v1\n"
      << "  --threads <Thread Numbers, default 1>\n"
      << "  --time <Timeout secs, default 60 seconds>\n"
      << "  --type <audio type, default pcm>\n"
      << "  --log <logLevel, default LogDebug = 4, closeLog = 0>\n"
      << "  --sampleRate <sample rate, 16K or 8K>\n"
      << "  --long <long connection: 1, short connection: 0, default 0>\n"
      << "  --sys <use system getaddrinfo(): 1, evdns_getaddrinfo(): 0>\n"
      << "  --noSleep <use sleep after sendAudio(), default 0>\n"
      << "  --audioFile <the absolute path of audio file>\n"
      << "  --maxSilence <max silence time of sentence>\n"
      << "  --loop <loop count>\n"
      << "  --maxSilence <max sentence silence time>\n"
      << "  --NlsScan <profile scan number>\n"
      << "eg:\n"
      << "  ./stDemo --appkey xxxxxx --token xxxxxx\n"
      << "  ./stDemo --appkey xxxxxx --token xxxxxx --threads 4 --time 3600\n"
      << "  ./stDemo --appkey xxxxxx --token xxxxxx --threads 4 --time 3600 --log 4 --type pcm\n"
      << "  ./stDemo --appkey xxxxxx --token xxxxxx --threads 1 --loop 1 --log 4 --type pcm --audioFile /home/xxx/test0.wav \n"
      << "  ./stDemo --appkey xxxxxx --akId xxxxxx --akSecret xxxxxx --threads 4 --time 3600\n"
      << std::endl;
    return -1;
  }

  signal(SIGINT, signal_handler_int);
  signal(SIGQUIT, signal_handler_quit);

  std::cout << " appKey: " << g_appkey << std::endl;
  std::cout << " akId: " << g_akId << std::endl;
  std::cout << " akSecret: " << g_akSecret << std::endl;
  std::cout << " domain for token: " << g_domain << std::endl;
  std::cout << " apiVersion for token: " << g_api_version << std::endl;
  std::cout << " threads: " << g_threads << std::endl;
  if (!g_audio_path.empty()) {
    std::cout << " audio files path: " << g_audio_path << std::endl;
  }
  std::cout << " loop timeout: " << loop_timeout << std::endl;
  std::cout << " loop count: " << loop_count << std::endl;
  std::cout << "\n" << std::endl;

  pthread_mutex_init(&params_mtx, NULL);

  if (profile_scan > 0) {
    g_sys_info = new PROFILE_INFO[profile_scan + 1];
    memset(g_sys_info, 0, sizeof(PROFILE_INFO) * (profile_scan + 1));

    // 启动 profile扫描, 同时关闭sys数据打印
    global_sys = false;
  } else {
    // 不进行性能扫描时, profile_scan赋为0, cur_profile_scan默认-1,
    // 即后续只跑一次startWorkThread
    profile_scan = 0;
  }

  for (cur_profile_scan = -1;
       cur_profile_scan < profile_scan;
       cur_profile_scan++) {

    if (cur_profile_scan == 0) continue;

    // 根据需要设置SDK输出日志, 可选. 
    // 此处表示SDK日志输出至log-Transcriber.txt， LogDebug表示输出所有级别日志
    // 需要最早调用
    if (logLevel > 0) {
      int ret = AlibabaNls::NlsClient::getInstance()->setLogConfig(
        "log-transcriber", (AlibabaNls::LogLevel)logLevel, 400, 50);
      if (ret < 0) {
        std::cout << "set log failed." << std::endl;
        return -1;
      }
    }

    // 设置运行环境需要的套接口地址类型, 默认为AF_INET
    // 必须在startWorkThread()前调用
    //AlibabaNls::NlsClient::getInstance()->setAddrInFamily("AF_INET");

    // 私有云部署的情况下进行直连IP的设置
    // 必须在startWorkThread()前调用
    //AlibabaNls::NlsClient::getInstance()->setDirectHost("106.15.83.44");

    // 存在部分设备在设置了dns后仍然无法通过SDK的dns获取可用的IP,
    // 可调用此接口主动启用系统的getaddrinfo来解决这个问题.
    if (sysAddrinfo) {
      AlibabaNls::NlsClient::getInstance()->setUseSysGetAddrInfo(true);
    }

    std::cout << "startWorkThread begin... " << std::endl;

    // 启动工作线程, 在创建请求和启动前必须调用此函数
    // 入参为负时, 启动当前系统中可用的核数
    if (cur_profile_scan == -1) {
      // 高并发的情况下推荐4, 单请求的情况推荐为1
      // 若高并发CPU占用率较高, 则可填-1启用所有CPU核
      AlibabaNls::NlsClient::getInstance()->startWorkThread(g_cpu);
    } else {
      AlibabaNls::NlsClient::getInstance()->startWorkThread(cur_profile_scan);
    }

    std::cout << "startWorkThread finish" << std::endl;

    // 识别多个音频数据
    speechTranscriberMultFile(g_appkey.c_str(), g_threads);

    // 所有工作完成，进程退出前，释放nlsClient.
    // 请注意, releaseInstance()非线程安全.
    AlibabaNls::NlsClient::releaseInstance();

    int size = g_statistics.size();
    int success_count = 0;
    if (size > 0) {
      std::map<unsigned long, struct ParamStatistics *>::iterator it;
      std::cout << "\n" << std::endl;

      std::cout << "Threads count:" << g_threads
        << ", Requests count:" << run_cnt << std::endl;
      std::cout << "    success:" << run_success
        << " cancel:" << run_cancel
        << " fail:" << run_fail
        << " start failed:" << run_start_failed << std::endl;

      usleep(3000 * 1000);

      pthread_mutex_lock(&params_mtx);
      std::map<unsigned long, struct ParamStatistics *>::iterator iter;
      for (iter = g_statistics.begin(); iter != g_statistics.end();) {
        struct ParamStatistics *second = iter->second;
        if (second) {
          delete second;
          second = NULL;
        }
        g_statistics.erase(iter++);
      }
      g_statistics.clear();
      pthread_mutex_unlock(&params_mtx);
    }

    run_cnt = 0;
    run_start_failed = 0;
    run_success = 0;
    run_fail = 0;

    std::cout << "===============================" << std::endl;
  }  // for

  if (g_sys_info) {
    int k = 0;
    for (k = 0; k < profile_scan + 1; k++) {
      PROFILE_INFO *cur_info = &(g_sys_info[k]);
      if (k == 0) {
        std::cout << "WorkThread: " << k - 1
          << " USER: " << cur_info->usr_name
          << " CPU: " << cur_info->ave_cpu_percent << "% "
          << " MEM: " << cur_info->ave_mem_percent << "% "
          << " Average Time: " << cur_info->eAveTime << "ms"
          << std::endl;
      } else {
        std::cout << "WorkThread: " << k
          << " USER: " << cur_info->usr_name
          << " CPU: " << cur_info->ave_cpu_percent << "% "
          << " MEM: " << cur_info->ave_mem_percent << "% "
          << " Average Time: " << cur_info->eAveTime << "ms"
          << std::endl;
      }
    }

    delete[] g_sys_info;
    g_sys_info = NULL;
  }

  if (global_sys) {
    std::cout << "WorkThread: " << g_cpu << std::endl;
    std::cout << "  USER: " << g_ave_percent.usr_name << std::endl;
    std::cout << "    Min: " << std::endl;
    std::cout << "      CPU: " << g_min_percent.ave_cpu_percent
      << " %" << std::endl;
    std::cout << "      MEM: " << g_min_percent.ave_mem_percent
      << " %" << std::endl;
    std::cout << "    Max: " << std::endl;
    std::cout << "      CPU: " << g_max_percent.ave_cpu_percent
      << " %" << std::endl;
    std::cout << "      MEM: " << g_max_percent.ave_mem_percent
      << " %" << std::endl;
    std::cout << "    Average: " << std::endl;
    std::cout << "      CPU: " << g_ave_percent.ave_cpu_percent
      << " %" << std::endl;
    std::cout << "      MEM: " << g_ave_percent.ave_mem_percent
      << " %" << std::endl;
    std::cout << "===============================" << std::endl;
  }

  pthread_mutex_destroy(&params_mtx);

  return 0;
}