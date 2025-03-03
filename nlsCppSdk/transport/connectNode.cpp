﻿/*
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

#ifdef _MSC_VER
#include <Rpc.h>
#else
#include "uuid/uuid.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <iostream>

#if defined(__ANDROID__) || defined(__linux__)
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#ifndef __ANDRIOD__
#include <iconv.h>
#endif
#endif

#ifdef __GNUC__
#include <netdb.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#endif

#include "nlsGlobal.h"
#include "nlsClient.h"
#include "iNlsRequest.h"
#include "iNlsRequestParam.h"
#include "nlog.h"
#include "utility.h"
#include "workThread.h"
#include "nodeManager.h"
#include "connectNode.h"
#ifdef ENABLE_REQUEST_RECORDING
#include "text_utils.h"
#include "json/json.h"
#endif

namespace AlibabaNls {

#define NODE_FRAME_SIZE 2048

ConnectNode::ConnectNode(INlsRequest* request,
                         HandleBaseOneParamWithReturnVoid<NlsEvent>* handler,
                         bool isLongConnection) : _request(request), _handler(handler) {
  _dnsRequest = NULL;
  _dnsRequestCallbackStatus = 0;
  _retryConnectCount = 0;

  _socketFd = INVALID_SOCKET;

  _binaryEvBuffer = evbuffer_new();
  if (_binaryEvBuffer == NULL) {
    LOG_ERROR("_binaryEvBuffer is nullptr");
  }
  evbuffer_enable_locking(_binaryEvBuffer, NULL);

  _readEvBuffer = evbuffer_new();
  if (_readEvBuffer == NULL) {
    LOG_ERROR("_readEvBuffer is nullptr");
  }
  _cmdEvBuffer = evbuffer_new();
  if (_cmdEvBuffer == NULL) {
    LOG_ERROR("_cmdEvBuffer is nullptr");
  }
  _wwvEvBuffer = evbuffer_new();
  if (_wwvEvBuffer == NULL) {
    LOG_ERROR("_wwvEvBuffer is nullptr");
  }

  evbuffer_enable_locking(_readEvBuffer, NULL);
  evbuffer_enable_locking(_cmdEvBuffer, NULL);
  evbuffer_enable_locking(_wwvEvBuffer, NULL);

  _nlsEncoder = NULL; //createNlsEncoder
  _encoderType = ENCODER_NONE;
  _audioFrame = NULL;
  _audioFrameSize = 0;
  _maxFrameSize = 0;
  _isFrirstAudioFrame = true;

  _eventThread = NULL;

  _isDestroy = false;
  _isWakeStop = false;
  _isStop = false;
  _isFirstBinaryFrame = true;

  _isLongConnection = isLongConnection;
  _isConnected = false;

  _workStatus = NodeCreated;
  _exitStatus = ExitInvalid;

  _instance = NULL;
  _syncCallTimeoutMs = 0;

#ifndef _MSC_VER
  _nodename = NULL;
  _servname = NULL;
  _dnsThread = 0;
  _dnsThreadExit = false;
  _dnsThreadRunning = false;
  _gaicbRequest[0] = NULL;
  _dnsEvent = NULL;
  _dnsErrorCode = 0;
  _addrinfo = NULL;
#endif

  _sslHandle = new SSLconnect();
  if (_sslHandle == NULL) {
    LOG_ERROR("Node(%p) _sslHandle is nullptr.", this);
  } else {
    LOG_DEBUG("Node(%p) new SSLconnect:%p.", this, _sslHandle);
  }

  LOG_INFO("Node(%p) create ConnectNode include webSocketTcp:%p.", this, &_webSocket);

  // will update parameters in updateParameters()
  int timeout_ms = request->getRequestParam()->getTimeout();
  _connectTv.tv_sec = timeout_ms / 1000;
  _connectTv.tv_usec = (timeout_ms - _connectTv.tv_sec * 1000) * 1000;

  _enableRecvTv = request->getRequestParam()->getEnableRecvTimeout();
  timeout_ms = request->getRequestParam()->getRecvTimeout();
  _recvTv.tv_sec = timeout_ms / 1000;
  _recvTv.tv_usec = (timeout_ms - _recvTv.tv_sec * 1000) * 1000;

  timeout_ms = request->getRequestParam()->getSendTimeout();
  _sendTv.tv_sec = timeout_ms / 1000;
  _sendTv.tv_usec = (timeout_ms - _sendTv.tv_sec * 1000) * 1000;

  _enableOnMessage = request->getRequestParam()->getEnableOnMessage();

  _launchEvent = NULL;
  _connectEvent = NULL;
  _readEvent = NULL;
  _writeEvent = NULL;
#ifdef ENABLE_HIGH_EFFICIENCY
  _connectTimerTv.tv_sec = 0;
  _connectTimerTv.tv_usec = CONNECT_TIMER_INVERVAL_MS * 1000;
  _connectTimerFlag = true;
  _connectTimerEvent = NULL;
#endif

#if defined(_MSC_VER)
  _mtxNode = CreateMutex(NULL, FALSE, NULL);
  _mtxCloseNode = CreateMutex(NULL, FALSE, NULL);
  _mtxEventCallbackNode = CreateEvent(NULL, FALSE, FALSE, NULL);
  _mtxInvokeSyncCallNode = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
  pthread_mutex_init(&_mtxNode, NULL);
  pthread_mutex_init(&_mtxCloseNode, NULL);
  pthread_mutex_init(&_mtxEventCallbackNode, NULL);
  pthread_mutex_init(&_mtxInvokeSyncCallNode, NULL);
  pthread_cond_init(&_cvEventCallbackNode, NULL);
  pthread_cond_init(&_cvInvokeSyncCallNode, NULL);
#endif
  _inEventCallbackNode = false;
  _nodeUUID = getRandomUuid();

#ifdef ENABLE_REQUEST_RECORDING
  _nodeProcess.last_status = NodeCreated;
  _nodeProcess.create_timestamp_ms = utility::TextUtils::GetTimestampMs();
  _nodeProcess.last_op_timestamp_ms = _nodeProcess.create_timestamp_ms;
#endif

  LOG_INFO("Node(%p) create ConnectNode done with long connection flag:%d, the UUID is %s",
    this, _isLongConnection, _nodeUUID.c_str());
}

ConnectNode::~ConnectNode() {
  LOG_DEBUG("Node(%p) destroy ConnectNode begin.", this);

#ifndef _MSC_VER
  int ret = 0;
  if (_url._enableSysGetAddr) {
    if (_dnsThread) {
      LOG_WARN("Node(%p) dnsThread(%lu) still exist, waiting exiting", this, _dnsThread);
      _dnsThreadExit = true;
      #ifndef __ANDROID__
      pthread_join(_dnsThread, NULL);
      #endif
      LOG_WARN("Node(%p) dnsThread(%lu) exited.", this, _dnsThread);
      _dnsThread = 0;
      if (_gaicbRequest[0]) {
        free(_gaicbRequest[0]);
        _gaicbRequest[0] = NULL;
      }
    } else {
      LOG_DEBUG("Node(%p) dnsThread has exited.", this);
    }
  }
#endif

  closeConnectNode();
  _eventThread->freeListNode(_eventThread, _request);

  _request = NULL;

  if (_sslHandle) {
    LOG_INFO("Node(%p) delete _sslHandle:%p.", this, _sslHandle);
    delete _sslHandle;
    _sslHandle = NULL;
  }

  _handler = NULL;

  if (_cmdEvBuffer) {
    evbuffer_free(_cmdEvBuffer);
    _cmdEvBuffer = NULL;
  }
  if (_readEvBuffer) {
    evbuffer_free(_readEvBuffer);
    _readEvBuffer = NULL;
  }
  if (_binaryEvBuffer) {
    evbuffer_free(_binaryEvBuffer);
    _binaryEvBuffer = NULL;
  }
  if (_wwvEvBuffer) {
    evbuffer_free(_wwvEvBuffer);
    _wwvEvBuffer = NULL;
  }
  if (_launchEvent) {
    event_free(_launchEvent);
    _launchEvent = NULL;
  }

  if (_dnsRequest && _dnsRequestCallbackStatus == 1) {
    LOG_DEBUG("Node(%p) cancel _dnsRequest(%p), current event_count_active_added_virtual:%d",
        this, _dnsRequest,
        event_base_get_num_events(
          _eventThread->_workBase, EVENT_BASE_COUNT_ACTIVE |
          EVENT_BASE_COUNT_ADDED | EVENT_BASE_COUNT_VIRTUAL));
    LOG_DEBUG("Node(%p) cancel _dnsRequest(%p), current event_count_active:%d",
        this, _dnsRequest,
        event_base_get_num_events(
          _eventThread->_workBase, EVENT_BASE_COUNT_ACTIVE));
    LOG_DEBUG("Node(%p) cancel _dnsRequest(%p), current event_count_virtual:%d",
        this, _dnsRequest,
        event_base_get_num_events(
          _eventThread->_workBase, EVENT_BASE_COUNT_VIRTUAL));
    LOG_DEBUG("Node(%p) cancel _dnsRequest(%p), current event_count_added:%d",
        this, _dnsRequest,
        event_base_get_num_events(
          _eventThread->_workBase, EVENT_BASE_COUNT_ADDED));
    evdns_getaddrinfo_cancel(_dnsRequest);
  }
  LOG_DEBUG("Node(%p) get event_count_active_added_virtual %d in deconstructing ConnectNode.",
      this, event_base_get_num_events(
        _eventThread->_workBase, EVENT_BASE_COUNT_ACTIVE |
        EVENT_BASE_COUNT_ADDED | EVENT_BASE_COUNT_VIRTUAL));
  LOG_DEBUG("Node(%p) get event_count_active %d in deconstructing ConnectNode.",
      this, event_base_get_num_events(
        _eventThread->_workBase, EVENT_BASE_COUNT_ACTIVE));
  LOG_DEBUG("Node(%p) get event_count_virtual %d in deconstructing ConnectNode.",
      this, event_base_get_num_events(
        _eventThread->_workBase, EVENT_BASE_COUNT_VIRTUAL));
  LOG_DEBUG("Node(%p) get event_count_added %d in deconstructing ConnectNode.",
      this, event_base_get_num_events(
        _eventThread->_workBase, EVENT_BASE_COUNT_ADDED));

  if (_nlsEncoder) {
    _nlsEncoder->destroyNlsEncoder();
    delete _nlsEncoder;
    _nlsEncoder = NULL;
  }
  if (_audioFrame) {
    free(_audioFrame);
    _audioFrame = NULL;
  }
  _audioFrameSize = 0;
  _maxFrameSize = 0;
  _isFrirstAudioFrame = true;

#if defined(_MSC_VER)
  CloseHandle(_mtxNode);
  CloseHandle(_mtxCloseNode);
  CloseHandle(_mtxEventCallbackNode);
  CloseHandle(_mtxInvokeSyncCallNode);
#else
  pthread_mutex_destroy(&_mtxNode);
  pthread_mutex_destroy(&_mtxCloseNode);
  pthread_mutex_destroy(&_mtxEventCallbackNode);
  pthread_mutex_destroy(&_mtxInvokeSyncCallNode);
  pthread_cond_destroy(&_cvEventCallbackNode);
  pthread_cond_destroy(&_cvInvokeSyncCallNode);
#endif
  _inEventCallbackNode = false;

  _instance = NULL;

  LOG_DEBUG("Node(%p) destroy ConnectNode done.", this);
}

void ConnectNode::initAllStatus() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  _isFirstBinaryFrame = true;
  _isStop = false;
  _isDestroy = false;
  _isWakeStop = false;

  _workStatus = NodeCreated;
  _exitStatus = ExitInvalid;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

struct event * ConnectNode::getLaunchEvent(bool init) {
  if (_launchEvent == NULL) {
    _launchEvent = event_new(_eventThread->_workBase, -1, EV_READ,
                             WorkThread::launchEventCallback, this);
    if (NULL == _launchEvent) {
      LOG_ERROR("Node(%p) new event(_launchEvent) failed.", this);
    } else {
      LOG_DEBUG("Node(%p) new event(_launchEvent).", this);
    }
  } else {
    if (init) {
      event_del(_launchEvent);
      event_assign(_launchEvent, _eventThread->_workBase, -1,
                   EV_READ,
                   WorkThread::launchEventCallback,
                   this);
      LOG_DEBUG("Node(%p) new event_assign(_launchEvent).", this);
    }
  }
  return _launchEvent;
}

ConnectStatus ConnectNode::getConnectNodeStatus() {
  ConnectStatus status;

#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  status = _workStatus;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
  return status;
}

std::string ConnectNode::getConnectNodeStatusString(ConnectStatus status) {
  std::string ret_str("Unknown");
  switch (status) {
    case NodeInvalid:
      ret_str.assign("NodeInvalid");
      break;
    case NodeCreated:
      ret_str.assign("NodeCreated");
      break;
    case NodeInvoking:
      ret_str.assign("NodeInvoking");
      break;
    case NodeInvoked:
      ret_str.assign("NodeInvoked");
      break;
    case NodeConnecting:
      ret_str.assign("NodeConnecting");
      break;
    case NodeConnected:
      ret_str.assign("NodeConnected");
      break;
    case NodeHandshaking:
      ret_str.assign("NodeHandshaking");
      break;
    case NodeHandshaked:
      ret_str.assign("NodeHandshaked");
      break;
    case NodeStarting:
      ret_str.assign("NodeStarting");
      break;
    case NodeStarted:
      ret_str.assign("NodeStarted");
      break;
    case NodeWakeWording:
      ret_str.assign("NodeWakeWording");
      break;
    case NodeFailed:
      ret_str.assign("NodeFailed");
      break;
    case NodeCompleted:
      ret_str.assign("NodeCompleted");
      break;
    case NodeClosed:
      ret_str.assign("NodeClosed");
      break;
    case NodeReleased:
      ret_str.assign("NodeReleased");
      break;
    case NodeStop:
      ret_str.assign("NodeStop");
      break;
    case NodeCancel:
      ret_str.assign("NodeCancel");
      break;
    case NodeSendAudio:
      ret_str.assign("NodeSendAudio");
      break;
    case NodeSendControl:
      ret_str.assign("NodeSendControl");
      break;
    case NodePlayAudio:
      ret_str.assign("NodePlayAudio");
      break;
    default:
      LOG_ERROR("Current invalid node status:%d.", status);
  }
  return ret_str;
}

std::string ConnectNode::getConnectNodeStatusString() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  std::string ret_str = getConnectNodeStatusString(_workStatus);

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
  return ret_str;
}

void ConnectNode::setConnectNodeStatus(ConnectStatus status) {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  _workStatus = status;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

ExitStatus ConnectNode::getExitStatus() {
  ExitStatus ret = ExitInvalid;

#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  ret = _exitStatus;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return ret;
}

std::string ConnectNode::getExitStatusString() {
  std::string ret_str = "Unknown";
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  switch (_exitStatus) {
    case ExitInvalid:
      ret_str.assign("ExitInvalid");
      break;
    case ExitStopping:
      ret_str.assign("ExitStopping");
      break;
    case ExitCancel:
      ret_str.assign("ExitCancel");
      break;
    default:
      LOG_ERROR("Current invalid exit status:%d.", _exitStatus);
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return ret_str;
}

void ConnectNode::setExitStatus(ExitStatus status) {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  _exitStatus = status;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

std::string ConnectNode::getCmdTypeString(int type) {
  std::string ret_str = "Unknown";

  switch (type) {
    case CmdStart:
      ret_str.assign("CmdStart");
      break;
    case CmdStop:
      ret_str.assign("CmdStop");
      break;
    case CmdStControl:
      ret_str.assign("CmdStControl");
      break;
    case CmdTextDialog:
      ret_str.assign("CmdTextDialog");
      break;
    case CmdExecuteDialog:
      ret_str.assign("CmdExecuteDialog");
      break;
    case CmdWarkWord:
      ret_str.assign("CmdWarkWord");
      break;
    case CmdCancel:
      ret_str.assign("CmdCancel");
      break;
  }

  return ret_str;
}

bool ConnectNode::updateDestroyStatus() {
  bool ret = true;
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  if (!_isDestroy) {
  #ifndef _MSC_VER
    int result = 0;
    if (_url._enableSysGetAddr) {
      if (_dnsThread) {
        LOG_WARN("Node(%p) dnsThread(%lu) still exist, waiting exiting", this, _dnsThread);
        _dnsThreadExit = true;
        #ifndef __ANDROID__
        pthread_join(_dnsThread, NULL);
        #endif
        LOG_WARN("Node(%p) dnsThread(%lu) exited.", this, _dnsThread);
        _dnsThread = 0;
      } else {
        LOG_DEBUG("Node(%p) dnsThread has exited.", this);
      }
    }
  #endif

    _isDestroy = true;
    ret = false;
  } else {
    LOG_DEBUG("Node(%p) _isDestroy is true, do nothing ...", this);
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return ret;
}

void ConnectNode::setWakeStatus(bool status) {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  _isWakeStop = status;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

bool ConnectNode::getWakeStatus() {
  bool ret = false;
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  ret = _isWakeStop;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return ret;
}

bool ConnectNode::checkConnectCount() {
  bool result = false;

#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  if (_retryConnectCount < RETRY_CONNECT_COUNT) {
    _retryConnectCount++;
    result = true;
  } else {
    _retryConnectCount = 0;
    // return false : restart connect failed
  }
  LOG_INFO("Node(%p) check connection count: %d.", this, _retryConnectCount);

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return result;
}

bool ConnectNode::parseUrlInformation(char *directIp) {
  if (_request == NULL) {
    LOG_ERROR("Node(%p) this request is nullptr.", this);
    return false;
  }

  const char* address = _request->getRequestParam()->_url.c_str();
  const char* token = _request->getRequestParam()->_token.c_str();
  size_t tokenSize = _request->getRequestParam()->_token.size();

  memset(&_url, 0x0, sizeof(struct urlAddress));

  if (directIp && strnlen(directIp, 64) > 0) {
    LOG_DEBUG("Node(%p) direct ip address: %s.", this, directIp);

    if (sscanf(directIp, "%[^:/]:%d", _url._address, &_url._port) == 2) {
      _url._directIp = true;
    } else if (sscanf(directIp, "%s", _url._address) == 1) {
      _url._directIp = true;
    } else {
      LOG_ERROR("Node(%p) could not parse WebSocket direct ip:%s.", this, directIp);
      return false;
    }
  }

  LOG_INFO("Node(%p) address: %s.", this, address);

  if (sscanf(address, "%[^:/]://%[^:/]:%d/%s", _url._type, _url._host, &_url._port, _url._path) == 4) {
    if (strcmp(_url._type, "wss") == 0 || strcmp(_url._type, "https") == 0) {
      _url._isSsl = true;
    }
  } else if (sscanf(address, "%[^:/]://%[^:/]/%s", _url._type, _url._host, _url._path) == 3) {
    if (strcmp(_url._type, "wss") == 0 || strcmp(_url._type, "https") == 0) {
      _url._port = 443;
      _url._isSsl = true;
    } else {
      _url._port = 80;
    }
  } else if (sscanf(address, "%[^:/]://%[^:/]:%d", _url._type, _url._host, &_url._port) == 3) {
    _url._path[0] = '\0';
  } else if (sscanf(address, "%[^:/]://%[^:/]", _url._type, _url._host) == 2) {
    if (strcmp(_url._type, "wss") == 0 || strcmp(_url._type, "https") == 0) {
      _url._port = 443;
      _url._isSsl = true;
    } else {
      _url._port = 80;
    }
    _url._path[0] = '\0';
  } else {
    LOG_ERROR("Node(%p) could not parse WebSocket url: %s.", this, address);

    return false;
  }

  memcpy(_url._token, token, tokenSize);

  LOG_INFO("Node(%p) type:%s, host:%s, port:%d, path:%s.",
      this, _url._type, _url._host, _url._port, _url._path);

  return true;
}

/**
 * @brief: 关闭ssl并释放event, 调用后会进行重链操作
 * @return:
 */
void ConnectNode::disconnectProcess() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxCloseNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxCloseNode);
#endif

  LOG_DEBUG("Node(%p) disconnectProcess begin, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

  if (_socketFd != INVALID_SOCKET) {
    if (_url._isSsl) {
      _sslHandle->sslClose();
    }
    evutil_closesocket(_socketFd);
    _socketFd = INVALID_SOCKET;

    if (_url._enableSysGetAddr && _dnsEvent) {
      event_free(_dnsEvent);
      _dnsEvent = NULL;
    }
  }

  _isConnected = false;

  LOG_DEBUG("Node(%p) disconnectProcess done, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

#if defined(_MSC_VER)
  ReleaseMutex(_mtxCloseNode);
#else
  pthread_mutex_unlock(&_mtxCloseNode);
#endif
}

/**
 * @brief: 当前Node切换到close状态, 而不进行断网, 用于长链接模式
 * @return:
 */
void ConnectNode::closeStatusConnectNode() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxCloseNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxCloseNode);
#endif

  LOG_DEBUG("Node(%p) closeStatusConnectNode begin, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

  if (_nlsEncoder) {
    _nlsEncoder->nlsEncoderSoftRestart();
  }

  if (_audioFrame) {
    free(_audioFrame);
    _audioFrame = NULL;
  }
  _audioFrameSize = 0;
  _maxFrameSize = 0;
  _isFrirstAudioFrame = true;

  LOG_DEBUG("Node(%p) closeStatusConnectNode done, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

#if defined(_MSC_VER)
  ReleaseMutex(_mtxCloseNode);
#else
  pthread_mutex_unlock(&_mtxCloseNode);
#endif
}

/**
 * @brief: 关闭ssl并释放event, 并设置node状态, 调用后往往进行释放操作.
 * @return:
 */
void ConnectNode::closeConnectNode() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxCloseNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxCloseNode);
#endif

  LOG_DEBUG("Node(%p) closeConnectNode begin, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

  if (_socketFd != INVALID_SOCKET) {
    if (_url._isSsl) {
      _sslHandle->sslClose();
    }
    evutil_closesocket(_socketFd);
    _socketFd = INVALID_SOCKET;
  }

  _isConnected = false;

  if (_url._enableSysGetAddr && _dnsEvent) {
    event_del(_dnsEvent);
    event_free(_dnsEvent);
    _dnsEvent = NULL;
  }
  if (_readEvent) {
    event_del(_readEvent);
    event_free(_readEvent);
    _readEvent = NULL;
  }
  if (_writeEvent) {
    event_del(_writeEvent);
    event_free(_writeEvent);
    _writeEvent = NULL;
  }
  if (_connectEvent) {
    event_del(_connectEvent);
    event_free(_connectEvent);
    _connectEvent = NULL;
  }
  if (_launchEvent) {
    event_del(_launchEvent);
    event_free(_launchEvent);
    _launchEvent = NULL;
  }

#ifdef ENABLE_HIGH_EFFICIENCY
  if (_connectTimerEvent != NULL) {
    if (_connectTimerFlag) {
      evtimer_del(_connectTimerEvent);
      _connectTimerFlag = false;
    }
    event_free(_connectTimerEvent);
    _connectTimerEvent = NULL;
  }
#endif

  if (_audioFrame) {
    free(_audioFrame);
    _audioFrame = NULL;
  }
  _audioFrameSize = 0;
  _maxFrameSize = 0;
  _isFrirstAudioFrame = true;

  LOG_INFO("Node(%p) closeConnectNode done, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

#if defined(_MSC_VER)
  ReleaseMutex(_mtxCloseNode);
#else
  pthread_mutex_unlock(&_mtxCloseNode);
#endif
}

int ConnectNode::socketWrite(const uint8_t * buffer, size_t len) {
#if defined(__ANDROID__) || defined(__linux__)
  int wLen = send(_socketFd, (const char *)buffer, len, MSG_NOSIGNAL);
#else
  int wLen = send(_socketFd, (const char*)buffer, len, 0);
#endif

  if (wLen < 0) {
    int errorCode = utility::getLastErrorCode();
    if (NLS_ERR_RW_RETRIABLE(errorCode)) {
      //LOG_DEBUG("Node(%p) socketWrite continue.", this);

      return Success;
    } else {
      return -(SocketWriteFailed);
    }
  } else {
    return wLen;
  }
}

int ConnectNode::socketRead(uint8_t * buffer, size_t len) {
  int rLen = recv(_socketFd, (char*)buffer, len, 0);

  if (rLen <= 0) { //rLen == 0, close socket, need do
    int errorCode = utility::getLastErrorCode();
    if (NLS_ERR_RW_RETRIABLE(errorCode)) {
      //LOG_DEBUG("Node(%p) socketRead continue.", this);

      return Success;
    } else {
      return -(SocketReadFailed);
    }
  } else {
    return rLen;
  }
}

int ConnectNode::gatewayRequest() {
  REQUEST_CHECK(_request, this);
  if (NULL == _readEvent) {
    LOG_ERROR("Node(%p) _readEvent is nullptr.", this);
    return -(EventEmpty);
  }

  if (_enableRecvTv) {
    int timeout_ms = _request->getRequestParam()->getRecvTimeout();
    _recvTv.tv_sec = timeout_ms / 1000;
    _recvTv.tv_usec = (timeout_ms - _recvTv.tv_sec * 1000) * 1000;
    event_add(_readEvent, &_recvTv);
  } else {
    event_add(_readEvent, NULL);
  }

  char tmp[NODE_FRAME_SIZE] = {0};
  int tmpLen = _webSocket.requestPackage(
      &_url, tmp, _request->getRequestParam()->GetHttpHeader());
  if (tmpLen < 0) {
    LOG_DEBUG("Node(%p) WebSocket request string failed.", this);
    return -(GetHttpHeaderFailed);
  };

  evbuffer_add(_cmdEvBuffer, (void *)tmp, tmpLen);

  return Success;
}

/**
 * @brief: 获取gateway的响应
 * @return: 成功则返回收到的字节数, 失败则返回负值.
 */
int ConnectNode::gatewayResponse() {
  int ret = 0;
  int read_len = 0;
  uint8_t *frame = (uint8_t *)calloc(READ_BUFFER_SIZE, sizeof(char));
  if (frame == NULL) {
    LOG_ERROR("Node(%p) %s %d calloc failed.", this, __func__, __LINE__);
    return -(MallocFailed);
  }

  read_len = nlsReceive(frame, READ_BUFFER_SIZE);
  if (read_len < 0) {
    LOG_ERROR("Node(%p) nlsReceive failed, read_len:%d", this, read_len);
    free(frame);
    return -(NlsReceiveFailed);
  } else if (read_len == 0) {
    LOG_WARN("Node(%p) nlsReceive empty, read_len:%d", this, read_len);
    free(frame);
    return -(NlsReceiveEmpty);
  }

  int frameSize = evbuffer_get_length(_readEvBuffer);
  if (frameSize > READ_BUFFER_SIZE) {
    frame = (uint8_t *)realloc(frame, frameSize + 1);
    if (frame == NULL) {
      LOG_ERROR("Node(%p) %s %d realloc failed.", this, __func__, __LINE__);
      free(frame);
      return -(ReallocFailed);
    }
  }

  evbuffer_copyout(_readEvBuffer, frame, frameSize);  //evbuffer_peek

  ret = _webSocket.responsePackage((const char*)frame, frameSize);
  if (ret == 0) {
    evbuffer_drain(_readEvBuffer, frameSize);
  } else if (ret > 0) {
    LOG_DEBUG("Node(%p) GateWay Middle response: %d\n %s",
        this, frameSize, frame);
  } else {
    _nodeErrMsg = _webSocket.getFailedMsg();
    LOG_ERROR("Node(%p) webSocket.responsePackage: %s",
        this, _nodeErrMsg.c_str());
  }

  if (frame) free(frame);
  frame = NULL;

  return ret;
}

/**
 * @brief: 将buffer中剩余音频数据ws封包并发送
 * @return: 成功发送的字节数, 失败则返回负值.
 */
int ConnectNode::addRemainAudioData() {
  int ret = 0;
  if (_audioFrame != NULL && _audioFrameSize > 0) {
    ret = addAudioDataBuffer(_audioFrame, _audioFrameSize);
    memset(_audioFrame, 0, _maxFrameSize);
    _audioFrameSize = 0;
  }
  return ret;
}

/**
 * @brief: 将音频数据切片或填充后再ws封包并发送
 * @param frame	用户传入的数据
 * @param length	用户传入的数据字节数
 * @return: 成功发送的字节数(可能为0, 留下一包数据发送), 失败则返回负值.
 */
int ConnectNode::addSlicedAudioDataBuffer(const uint8_t * frame, size_t length) {
  int ret = 0;
  int filling_ret = 0;

  if (_nlsEncoder && _encoderType != ENCODER_NONE) {
    #ifdef ENABLE_NLS_DEBUG
    LOG_DEBUG("Node(%p) addSlicedAudioDataBuffer input data %d bytes.", this, length);
    #endif
    _maxFrameSize = _nlsEncoder->getFrameSampleBytes();
    if (_maxFrameSize <= 0) {
      return _maxFrameSize;
    }
    if (_audioFrame == NULL) {
        _audioFrame = (unsigned char*)calloc(_maxFrameSize, sizeof(unsigned char*));
        if (_audioFrame == NULL) {
          LOG_ERROR("Node(%p) malloc audio_data_buffer failed.", this);
          return -(MallocFailed);
        #ifdef ENABLE_NLS_DEBUG
        } else {
          LOG_DEBUG("Node(%p) create audio frame data %d bytes.", this, _maxFrameSize);
        #endif
        }
        _audioFrameSize = 0;
    }

    size_t buffer_space_size = 0; /*buffer中空闲空间, 最大为_maxFrameSize*/
    size_t frame_used_size = 0; /*frame已经传入buffer的字节数*/
    size_t frame_remain_size = length; /*frame未传入buffer的字节数*/
    do {
      buffer_space_size = _maxFrameSize - _audioFrameSize;
      if (frame_remain_size < buffer_space_size) {
        memcpy(_audioFrame + _audioFrameSize, frame + frame_used_size, frame_remain_size);
        _audioFrameSize += frame_remain_size;
        frame_used_size += frame_remain_size;
        frame_remain_size -= frame_remain_size;
      } else {
        memcpy(_audioFrame + _audioFrameSize, frame + frame_used_size, buffer_space_size);
        _audioFrameSize += buffer_space_size;
        frame_used_size += buffer_space_size;
        frame_remain_size -= buffer_space_size;
      }

      if (_audioFrameSize >= _maxFrameSize) {
        /*每次填充完整的一包数据*/
        ret = addAudioDataBuffer(_audioFrame, _maxFrameSize);
        memset(_audioFrame, 0, _maxFrameSize);
        filling_ret += _maxFrameSize;
        _audioFrameSize = 0;
        #ifdef ENABLE_NLS_DEBUG
        LOG_DEBUG("Node(%p) ready to push audio data(%d) into nls_buffer, has digest %dbytes, ret:%d.",
              this, _maxFrameSize, frame_used_size, ret);
        #endif
        if (ret < 0) {
          filling_ret = ret;
          break;
        }
      } else {
        if (_isFrirstAudioFrame == false && _encoderType != ENCODER_OPU) {
          /*数据不足一包, 且非第一包数据. OPU第一包如果未满, 则会编码失败*/
          ret = addAudioDataBuffer(_audioFrame, _audioFrameSize);
          filling_ret += _audioFrameSize;
          #ifdef ENABLE_NLS_DEBUG
          LOG_DEBUG("Node(%p) ready to push audio data(%d) into nls_buffer, has digest %dbytes, ret:%d.",
                this, _audioFrameSize, frame_used_size, ret);
          #endif
          memset(_audioFrame, 0, _maxFrameSize);
          _audioFrameSize = 0;
          if (ret < 0) {
            filling_ret = ret;
            break;
          }
        } else {
          #ifdef ENABLE_NLS_DEBUG
          LOG_DEBUG("Node(%p) leave audio data(%d) for the next round.",
                this, _audioFrameSize);
          #endif
          break;
        }
      }
    } while (frame_used_size < length);
  } else {
    filling_ret = addAudioDataBuffer(frame, length);
  }

  #ifdef ENABLE_NLS_DEBUG
  LOG_DEBUG("Node(%p) pushed audio data(%d:%d) into audio_data_tmp_buffer.", this, length, filling_ret);
  #endif
  return filling_ret;
}

/**
 * @brief: 将音频数据进行ws封包并发送
 * @param frame	用户传入的数据
 * @param frameSize	用户传入的数据字节数
 * @return: 成功发送的字节数(可能为0, 留下一包数据发送), 失败则返回负值.
 */
int ConnectNode::addAudioDataBuffer(const uint8_t * frame, size_t frameSize) {
  REQUEST_CHECK(_request, this);
  int ret = 0;
  uint8_t *tmp = NULL;
  size_t tmpSize = 0;
  size_t length = 0;
  struct evbuffer* buff = NULL;

  if (_nlsEncoder && _encoderType != ENCODER_NONE) {
    int nSize = 0;
    uint8_t *outputBuffer = new uint8_t[frameSize];
    if (outputBuffer == NULL) {
      LOG_ERROR("Node(%p) new outputBuffer failed.", this);
      return -(NewOutputBufferFailed);
    } else {
      memset(outputBuffer, 0, frameSize);
      nSize = _nlsEncoder->nlsEncoding(
          frame, (int)frameSize,
          outputBuffer, (int)frameSize);
      #ifdef ENABLE_NLS_DEBUG
      LOG_DEBUG("Node(%p) Opus encoder(%d) encoding %dbytes data, and return nSize:%d.",
          this, _encoderType, frameSize, nSize);
      #endif
      if (nSize < 0) {
        LOG_ERROR("Node(%p) Opus encoder failed:%d.", this, nSize);
        delete [] outputBuffer;
        return -(NlsEncodingFailed);
      }
      _webSocket.binaryFrame(outputBuffer, nSize, &tmp, &tmpSize);
      delete [] outputBuffer;
    }
  } else {
    // pack frame data
    _webSocket.binaryFrame(frame, frameSize, &tmp, &tmpSize);
  }

  if (_request && _request->getRequestParam()->_enableWakeWord == true &&
      !getWakeStatus()) {
    buff = _wwvEvBuffer;
  } else {
    buff = _binaryEvBuffer;
  }

  evbuffer_lock(buff);
  length = evbuffer_get_length(buff);
  if (length >= _limitSize) {
    LOG_WARN("Too many audio data in evbuffer.");
    evbuffer_unlock(buff);
    return -(EvbufferTooMuch);
  }

  evbuffer_add(buff, (void *)tmp, tmpSize);

  if (tmp) free(tmp);
  tmp = NULL;

  evbuffer_unlock(buff);

  if (length == 0 && _workStatus == NodeStarted) {
    #if defined(_MSC_VER)
    WaitForSingleObject(_mtxNode, INFINITE);
    #else
    pthread_mutex_lock(&_mtxNode);
    #endif
    if (!_isStop) {
      ret = nlsSendFrame(buff);
    }
    #if defined(_MSC_VER)
    ReleaseMutex(_mtxNode);
    #else
    pthread_mutex_unlock(&_mtxNode);
    #endif
  }

  if (length == 0 && _workStatus == NodeWakeWording) {
    #if defined(_MSC_VER)
    WaitForSingleObject(_mtxNode, INFINITE);
    #else
    pthread_mutex_lock(&_mtxNode);
    #endif
    if (!_isStop) {
      ret = nlsSendFrame(buff);
    }
    #if defined(_MSC_VER)
    ReleaseMutex(_mtxNode);
    #else
    pthread_mutex_unlock(&_mtxNode);
    #endif
  }

  if (ret == 0) {
    ret = sendControlDirective();
  }

  if (ret < 0) {
    disconnectProcess();
    handlerTaskFailedEvent(getErrorMsg());
  } else {
    ret = frameSize;
    _isFrirstAudioFrame = false;
  }

  return ret;
}

/**
 * @brief: 设置同步调用模式的超时时间, 0则为关闭同步模式, 默认0,
 *         此模式start()后收到服务端结果再return出去,
 *         stop()后收到close()回调再return出去.
 * @param timeout_ms	大于0, 则启动同步调用模式
 * @return:
 */
void ConnectNode::setSyncCallTimeout(unsigned int timeout_ms) {
  _syncCallTimeoutMs = timeout_ms;
}

/**
 * @brief: 发送控制命令
 * @return: 成功发送的字节数, 失败则返回负值.
 */
int ConnectNode::sendControlDirective() {
  int ret = 0;

#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  if (_workStatus == NodeStarted && _exitStatus == ExitInvalid) {
    size_t length = evbuffer_get_length(getCmdEvBuffer());
    if (length != 0) {
      // LOG_DEBUG("Node(%p) cmd buffer isn't empty.", this);
      ret = nlsSendFrame(getCmdEvBuffer());
    }
  } else {
    if (_exitStatus == ExitStopping && _isStop == false) {
      // LOG_DEBUG("Node(%p) audio has send done. And invoke stop command.", this);
      addCmdDataBuffer(CmdStop);
      ret = nlsSendFrame(getCmdEvBuffer());
      _isStop = true;
    }
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif

  return ret;
}

/**
 * @brief: 将命令字符串加入buffer用于发送
 * @param type	CmdType
 * @param message	待发送命令
 * @return:
 */
void ConnectNode::addCmdDataBuffer(CmdType type, const char* message) {
  char *cmd = NULL;

  LOG_DEBUG("Node(%p) get command type: %s.", this, getCmdTypeString(type).c_str());

  if (_request == NULL) {
    LOG_ERROR("The rquest of node(%p) is nullptr.", this);
    return;
  }
  if (_request->getRequestParam() == NULL) {
    LOG_ERROR("The requestParam of request(%p) node(%p) is nullptr.", _request, this);
    return;
  }

  switch(type) {
    case CmdStart:
      cmd = (char *)_request->getRequestParam()->getStartCommand();
      break;
    case CmdStControl:
      cmd = (char *)_request->getRequestParam()->getControlCommand(message);
      break;
    case CmdStop:
      cmd = (char *)_request->getRequestParam()->getStopCommand();
      break;
    case CmdTextDialog:
      cmd = (char *)_request->getRequestParam()->getExecuteDialog();
      break;
    case CmdWarkWord:
      cmd = (char *)_request->getRequestParam()->getStopWakeWordCommand();
      break;
    case CmdCancel:
      LOG_DEBUG("Node(%p) add cancel command, do nothing.", this);
      return;
    default:
      LOG_WARN("Node(%p) add unknown command, do nothing.", this);
      return;
  }

  if (cmd) {
    std::string buf_str;
    LOG_INFO("Node(%p) get command: %s, and add into evbuffer.", this, cmdConvertForLog(cmd, &buf_str));

    uint8_t *frame = NULL;
    size_t frameSize = 0;
    _webSocket.textFrame((uint8_t *) cmd, strlen(cmd), &frame, &frameSize);

    evbuffer_add(_cmdEvBuffer, (void *) frame, frameSize);

    if (frame) free(frame);
    frame = NULL;
  }
}

/**
 * @brief: 命令发送
 * @param type	CmdType
 * @param message	待发送命令
 * @return: 成功则返回发送字节数, 失败则返回负值
 */
int ConnectNode::cmdNotify(CmdType type, const char* message) {
  int ret = Success;

  LOG_DEBUG("Node(%p) invoke CmdNotify: %s.", this, getCmdTypeString(type).c_str());

  if (type == CmdStop) {
#ifdef ENABLE_REQUEST_RECORDING
    _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
    _nodeProcess.stop_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
    _nodeProcess.last_status = NodeStop;
#endif
    addRemainAudioData();
    _exitStatus = ExitStopping;
    if (_workStatus == NodeStarted) {
      size_t length = evbuffer_get_length(_binaryEvBuffer);
      if (length == 0) {
        ret = sendControlDirective();
      } else {
        LOG_DEBUG("Node(%p) invoke CmdNotify: %s, and continue send audio data %zubytes.",
            this, getCmdTypeString(type).c_str(), length);
      }
    }
  } else if (type == CmdCancel) {
#ifdef ENABLE_REQUEST_RECORDING
    _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
    _nodeProcess.cancel_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
    _nodeProcess.last_status = NodeCancel;
#endif
    _exitStatus = ExitCancel;
  } else if (type == CmdStControl) {
#ifdef ENABLE_REQUEST_RECORDING
    _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
    _nodeProcess.last_ctrl_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
    _nodeProcess.last_status = NodeSendControl;
#endif
    addCmdDataBuffer(CmdStControl, message);
    if (_workStatus == NodeStarted) {
      size_t length = evbuffer_get_length(_binaryEvBuffer);
      if (length == 0) {
        ret = nlsSendFrame(_cmdEvBuffer);
      }
    }
  } else if (type == CmdWarkWord) {
    _isWakeStop = true;
    size_t length = evbuffer_get_length(_wwvEvBuffer);
    if (length == 0) {
      addCmdDataBuffer(CmdWarkWord);
      ret = nlsSendFrame(_cmdEvBuffer);
    }
  } else {
    LOG_ERROR("Node(%p) invoke unknown command.", this);
  }

  if (ret < 0) {
    disconnectProcess();
    handlerTaskFailedEvent(getErrorMsg());
  }

  return ret;
}

int ConnectNode::nlsSend(const uint8_t * frame, size_t length) {
  int sLen = 0;
  if ((frame == NULL) || (length == 0)) {
    return 0;
  }

  if (_url._isSsl) {
    sLen = _sslHandle->sslWrite(frame, length);
  } else {
    sLen = socketWrite(frame, length);
  }

  if (sLen < 0) {
    if (_url._isSsl) {
      _nodeErrMsg = _sslHandle->getFailedMsg();
    } else {
      _nodeErrMsg =
          evutil_socket_error_to_string(evutil_socket_geterror(_socketFd));
    }

    LOG_ERROR("Node(%p) send failed: %s.", this,  _nodeErrMsg.c_str());
  }

  return sLen;
}

/**
 * @brief: 发送一帧数据 
 * @return: 成功发送的字节数, 失败则返回负值.
 */
int ConnectNode::nlsSendFrame(struct evbuffer * eventBuffer) {
  int sLen = 0;
  uint8_t buffer[NODE_FRAME_SIZE] = {0};
  size_t bufferSize = 0;

  evbuffer_lock(eventBuffer);
  size_t length = evbuffer_get_length(eventBuffer);
  if (length == 0) {
    // LOG_DEBUG("Node(%p) eventBuffer is NULL.", this);
    evbuffer_unlock(eventBuffer);
    return 0;
  }

  if (length > NODE_FRAME_SIZE) {
    bufferSize = NODE_FRAME_SIZE;
  } else {
    bufferSize = length;
  }
  evbuffer_copyout(eventBuffer, buffer, bufferSize); //evbuffer_peek

  if (bufferSize > 0) {
    sLen = nlsSend(buffer, bufferSize);
  }

  if (sLen < 0) {
    LOG_ERROR("Node(%p) nlsSend failed, nlsSend return:%d.", this, sLen);
    evbuffer_unlock(eventBuffer);
    return -(NlsSendFailed);
  } else {
    evbuffer_drain(eventBuffer, sLen);
    length = evbuffer_get_length(eventBuffer);
    if (length > 0) {
      if (NULL == _writeEvent) {
        LOG_ERROR("Node(%p) event is nullptr.", this);
        evbuffer_unlock(eventBuffer);
        return -(EventEmpty);
      }
      int timeout_ms = _request->getRequestParam()->getSendTimeout();
      _sendTv.tv_sec = timeout_ms / 1000;
      _sendTv.tv_usec = (timeout_ms - _sendTv.tv_sec * 1000) * 1000;
      event_add(_writeEvent, &_sendTv);
    }
    evbuffer_unlock(eventBuffer);
    return length;
  }
}

int ConnectNode::nlsReceive(uint8_t *buffer, int max_size) {
  int rLen = 0;
  int read_buffer_size = max_size;
  if (_url._isSsl) {
    rLen = _sslHandle->sslRead((uint8_t *)buffer, read_buffer_size);
  } else {
    rLen = socketRead((uint8_t *)buffer, read_buffer_size);
  }

  if (rLen < 0) {
    if (_url._isSsl) {
      _nodeErrMsg = _sslHandle->getFailedMsg();
    } else {
      _nodeErrMsg = evutil_socket_error_to_string(
          evutil_socket_geterror(_socketFd));
    }
    LOG_ERROR("Node(%p) recv failed: %s.", this, _nodeErrMsg.c_str());
    return -(ReadFailed);
  }

  evbuffer_add(_readEvBuffer, (void *)buffer, rLen);

  return rLen;
}

/**
 * @brief: 接收一帧数据
 * @return: 成功接收的字节数, 失败则返回负值.
 */
int ConnectNode::webSocketResponse() {
  int ret = 0;
  int read_len = 0;
  uint8_t *frame = (uint8_t *)calloc(READ_BUFFER_SIZE, sizeof(char));
  if (frame == NULL) {
    LOG_ERROR("%s %d calloc failed.", __func__, __LINE__);
    return 0;
  }

  read_len = nlsReceive(frame, READ_BUFFER_SIZE);
  if (read_len < 0) {
    LOG_ERROR("Node(%p) nlsReceive failed, read_len:%d", this, read_len);
    free(frame);
    return -(NlsReceiveFailed);
  } else if (read_len == 0) {
    LOG_WARN("Node(%p) nlsReceive empty, read_len:%d", this, read_len);
    free(frame);
    return 0;
  }

  bool eLoop = false;
  size_t frameSize = 0;
  size_t cur_data_size = 0;
  do {
    ret = 0;
    frameSize = evbuffer_get_length(_readEvBuffer);
    if (frameSize <= 0) {
      free(frame);
      frame = NULL;
      ret = 0;
      break;
    } else if (frameSize > READ_BUFFER_SIZE) {
      frame = (uint8_t *)realloc(frame, frameSize + 1);
      if (frame == NULL) {
        LOG_ERROR("Node(%p) realloc failed.", this);
        free(frame);
        frame = NULL;
        ret = -(ReallocFailed);
        break;
      } else {
        LOG_WARN("Node(%p) websocket frame realloc, new size:%d.", this, frameSize + 1);
      }
    }

    cur_data_size = frameSize;
    evbuffer_copyout(_readEvBuffer, frame, frameSize);

    WebSocketFrame wsFrame;
    memset(&wsFrame, 0x0, sizeof(struct WebSocketFrame));
    if (_webSocket.receiveFullWebSocketFrame(
          frame, frameSize, &_wsType, &wsFrame) == Success) {
      // LOG_DEBUG("Node(%p) parse websocket frame, len:%zu, frame size:%zu.",
      //     this, wsFrame.length, frameSize);

      int result = parseFrame(&wsFrame);
      if (result) {
        LOG_ERROR("Node(%p) parse WS frame failed:%d.", this, result);
        ret = result;
        break;
      }

      evbuffer_drain(_readEvBuffer, wsFrame.length + _wsType.headerSize);
      cur_data_size = cur_data_size - (wsFrame.length + _wsType.headerSize);

      ret = wsFrame.length + _wsType.headerSize;
    }

    /* 解析成功并还有剩余数据, 则尝试再解析 */
    if (ret > 0 && cur_data_size > 0) {
      LOG_DEBUG("Node(%p) current data remainder size:%d, ret:%d, receive ws frame continue...",
          this, cur_data_size, ret);
      eLoop = true;
    } else {
      eLoop = false;
    }
  } while (eLoop);

  if (frame) free(frame);
  frame = NULL;

  return ret;
}

#if defined(__ANDROID__) || defined(__linux__)
int ConnectNode::codeConvert(char *from_charset, char *to_charset,
                             char *inbuf, size_t inlen,
                             char *outbuf, size_t outlen) {
#if defined(__ANDRIOD__)
  outbuf = inbuf;
#else
  iconv_t cd;
  char **pin = &inbuf;
  char **pout = &outbuf;

  cd = iconv_open(to_charset, from_charset);
  if (cd == 0) {
    return -(IconvOpenFailed);
  }

  memset(outbuf, 0, outlen);
  if (iconv(cd, pin, &inlen, pout, &outlen) == (size_t)-1) {
    return -(IconvFailed);
  }

  iconv_close(cd);
#endif
  return Success;
}
#endif

std::string ConnectNode::utf8ToGbk(const std::string &strUTF8) {
#if defined(__ANDROID__) || defined(__linux__)
  const char *msg = strUTF8.c_str();
  size_t inputLen = strUTF8.length();
  size_t outputLen = inputLen * 20;

  char *outbuf = new char[outputLen + 1];
  memset(outbuf, 0x0, outputLen + 1);

  char *inbuf = new char[inputLen + 1];
  memset(inbuf, 0x0, inputLen + 1);
  strncpy(inbuf, msg, inputLen);

  int res = codeConvert(
      (char *)"UTF-8", (char *)"GBK", inbuf, inputLen, outbuf, outputLen);
  if (res < 0) {
    LOG_ERROR("ENCODE: convert to utf8 error :%d .",
        utility::getLastErrorCode());
    return NULL;
  }

  std::string strTemp(outbuf);

  delete [] outbuf;
  outbuf = NULL;
  delete [] inbuf;
  inbuf = NULL;

  return strTemp;

#elif defined (_MSC_VER)

  int len = MultiByteToWideChar(CP_UTF8, 0, strUTF8.c_str(), -1, NULL, 0);
  unsigned short* wszGBK = new unsigned short[len + 1];
  memset(wszGBK, 0, len * 2 + 2);

  MultiByteToWideChar(CP_UTF8, 0, (char*)strUTF8.c_str(), -1, (wchar_t*)wszGBK, len);

  len = WideCharToMultiByte(CP_ACP, 0, (wchar_t*)wszGBK, -1, NULL, 0, NULL, NULL);

  char* szGBK = new char[len + 1];
  memset(szGBK, 0, len + 1);
  WideCharToMultiByte(CP_ACP, 0, (wchar_t*)wszGBK, -1, szGBK, len, NULL, NULL);

  std::string strTemp(szGBK);
  delete[] szGBK;
  delete[] wszGBK;

  return strTemp;

#else

  return strUTF8;

#endif
}

void ConnectNode::setInstance(NlsClient* instance) {
  _instance = instance;
}

NlsClient* ConnectNode::getInstance() {
  return _instance;
}

std::string ConnectNode::getNodeUUID() {
  return _nodeUUID;
}

NlsEvent* ConnectNode::convertResult(WebSocketFrame * wsFrame, int * ret) {
  NlsEvent* wsEvent = NULL;

  if (_request == NULL) {
    LOG_ERROR("Node(%p) this request is nullptr.", this);
    *ret = -(RequestEmpty);
    return NULL;
  }

  if (wsFrame->type == WebSocketHeaderType::BINARY_FRAME) {
    if (wsFrame->length > 0) {
      std::vector<unsigned char> data = std::vector<unsigned char>(
          wsFrame->data, wsFrame->data + wsFrame->length);

      wsEvent = new NlsEvent(data, 0,
                             NlsEvent::Binary,
                             _request->getRequestParam()->_task_id);
      if (wsEvent == NULL) {
        LOG_ERROR("Node(%p) new NlsEvent failed!", this);
        handlerEvent(TASKFAILED_NEW_NLSEVENT_FAILED,
                     MemNotEnough,
                     NlsEvent::TaskFailed,
                     _enableOnMessage);
        *ret = -(NewNlsEventFailed);
      }
    } else {
      LOG_WARN("Node(%p) this ws frame length is invalid %d.", wsFrame->length);
      *ret = -(WsFrameBodyEmpty);
      return NULL;
    }
  } else if (wsFrame->type == WebSocketHeaderType::TEXT_FRAME) {
    /* 打印这个string，可能会因为太长而崩溃 */
    std::string result((char *)wsFrame->data, wsFrame->length);
    if (wsFrame->length > 1024) {
      std::string part_result((char *)wsFrame->data, 1024);
      LOG_INFO("Node(%p) ws frame len:%d is too long, part response(1024): %s.",
          this, wsFrame->length, part_result.c_str());
    } else if (wsFrame->length == 0) {
      LOG_ERROR("Node(%p) ws frame len is zero!", this);
    } else {
      LOG_INFO("Node(%p) response(ws frame len:%d): %s",
          this, wsFrame->length, result.c_str());
    }

    if ("GBK" == _request->getRequestParam()->_outputFormat) {
      result = utf8ToGbk(result);
    }
    if (result.empty()) {
      LOG_ERROR("Node(%p) response result is empty!", this);
      handlerEvent(TASKFAILED_UTF8_JSON_STRING,
                   Utf8ConvertError,
                   NlsEvent::TaskFailed,
                   _enableOnMessage);
      *ret = -(WsResponsePackageEmpty);
      return NULL;
    }

    wsEvent = new NlsEvent(result);
    if (wsEvent == NULL) {
      LOG_ERROR("Node(%p) new NlsEvent failed!", this);
      handlerEvent(TASKFAILED_NEW_NLSEVENT_FAILED,
                   MemNotEnough,
                   NlsEvent::TaskFailed,
                   _enableOnMessage);
      *ret = -(NewNlsEventFailed);
    } else {
      *ret = wsEvent->parseJsonMsg(_enableOnMessage);
      if (*ret < 0) {
        LOG_ERROR("Node(%p) parseJsonMsg failed! ret:%d.", this, ret);
        delete wsEvent;
        wsEvent = NULL;
        handlerEvent(TASKFAILED_PARSE_JSON_STRING,
                     JsonStringParseFailed,
                     NlsEvent::TaskFailed,
                     _enableOnMessage);
      } else {
        // LOG_DEBUG("Node(%p) parseJsonMsg success.", this);
      }
    }
  } else {
    LOG_ERROR("Node(%p) unknow WebSocketHeaderType:%d", this, wsFrame->type);
    handlerEvent(TASKFAILED_WS_JSON_STRING,
                 UnknownWsHeadType,
                 NlsEvent::TaskFailed,
                 _enableOnMessage);
    *ret = -(UnknownWsFrameHeadType);
  }

  return wsEvent;
}

const char* ConnectNode::cmdConvertForLog(char *buf_in, std::string *buf_str) {
  char buf_out[BUFFER_SIZE] = {0};
  std::string tmp_str(buf_in);
  std::string find_key = "appkey\":\"";
  int step = 8;
  int pos1 = 0;
  int pos2 = tmp_str.find(find_key);
  strncpy(buf_out, buf_in, BUFFER_SIZE - 1);

  if (pos2 >= 0) {
    for (pos1 = pos2 + find_key.length();
         pos1 < pos2 + find_key.length() + step;
         pos1++) {
      buf_out[pos1] = 'Z';
    }
  }

  *buf_str = buf_out;
  return buf_str->c_str();
}

int ConnectNode::parseFrame(WebSocketFrame * wsFrame) {
  REQUEST_CHECK(_request, this);
  int result = Success;
  NlsEvent* frameEvent = NULL;

  if (wsFrame->type == WebSocketHeaderType::CLOSE) {
    if (wsFrame->closeCode == -1) {
      std::string msg((char *)wsFrame->data);
      char tmp_msg[2048] = {0};
      snprintf(tmp_msg, 2048 - 1, "{\"TaskFailed\":\"%s\"}", msg.c_str());
      std::string closeMsg = tmp_msg;

      LOG_ERROR("Node(%p) close msg:%s.", this, closeMsg.c_str());

      frameEvent = new NlsEvent(
          closeMsg.c_str(), wsFrame->closeCode,
          NlsEvent::TaskFailed, _request->getRequestParam()->_task_id);
      if (frameEvent == NULL) {
        LOG_ERROR("Node(%p) new NlsEvent failed!", this);
        handlerEvent(TASKFAILED_NEW_NLSEVENT_FAILED,
                     MemNotEnough,
                     NlsEvent::TaskFailed,
                     _enableOnMessage);
        return -(NewNlsEventFailed);
      }
    } else {
      LOG_ERROR("Node(%p) get invalid wsFrame closeCode:%d.", this, wsFrame->closeCode);
      handlerEvent(TASKFAILED_ERROR_CLOSE_STRING,
                   UnknownWsHeadType,
                   NlsEvent::TaskFailed,
                   _enableOnMessage);
      return -(InvalidWsFrameCloseCode);
    }
  } else {
    frameEvent = convertResult(wsFrame, &result);
  }

  if (frameEvent == NULL) {
    if (result == -(WsFrameBodyEmpty)) {
      LOG_WARN("Node(%p) convert result failed, result:%d. Maybe recv dirty data, skip here ...", this, result);
      return Success;
    } else {
      LOG_ERROR("Node(%p) convert result failed, result:%d.", this, result);
      closeConnectNode();
      if (result != Success) {
        handlerEvent(CLOSE_JSON_STRING,
                     CLOSE_CODE,
                     NlsEvent::Close,
                     _enableOnMessage);
      }
      return -(NlsEventEmpty);
    }
  }

  LOG_DEBUG("Node(%p) begin HandlerFrame, msg type:%s node status:%s exit status:%s.",
      this, frameEvent->getMsgTypeString().c_str(),
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

  // invoked cancel()
  if (_exitStatus == ExitCancel) {
    LOG_WARN("Node(%p) has been canceled.", this);
    if (frameEvent) delete frameEvent;
    frameEvent = NULL;
    return -(InvalidExitStatus);
  }

  result = handlerFrame(frameEvent);
  if (result) {
    if (frameEvent) delete frameEvent;
    frameEvent = NULL;
    return result;
  }

  LOG_DEBUG("Node(%p) HandlerFrame finish, current node status:%s, ready to set workStatus.",
      this, getConnectNodeStatusString().c_str());

  bool closeFlag = false;
  int msg_type = frameEvent->getMsgType();
  switch(msg_type) {
    case NlsEvent::RecognitionStarted:
    case NlsEvent::TranscriptionStarted:
      if (_request->getRequestParam()->_requestType != SpeechWakeWordDialog) {
        _workStatus = NodeStarted;
      } else {
        _workStatus = NodeWakeWording;
      }
      break;
    case NlsEvent::Close:
    case NlsEvent::RecognitionCompleted:
      _workStatus = NodeCompleted;
      if (_request->getRequestParam()->_mode == TypeDialog) {
        closeFlag = false;
      } else {
        closeFlag = true;
      }
      break;
    case NlsEvent::TaskFailed:
      _workStatus = NodeFailed;
      closeFlag = true;
      break;
    case NlsEvent::TranscriptionCompleted:
      _workStatus = NodeCompleted;
      closeFlag = true;
      break;
    case NlsEvent::SynthesisCompleted:
      _workStatus = NodeCompleted;
      closeFlag = true;
      break;
    case NlsEvent::DialogResultGenerated:
      closeFlag = true;
      break;
    case NlsEvent::WakeWordVerificationCompleted:
      _workStatus = NodeStarted;
      break;
    case NlsEvent::Binary:
      if (_isFirstBinaryFrame) {
        _isFirstBinaryFrame = false;
        _workStatus = NodeStarted;
      }
      break;
    default:
      closeFlag = false;
      break;
  }

  if (frameEvent) delete frameEvent;
  frameEvent = NULL;

  if (closeFlag) {
    if (!_isLongConnection || _workStatus == NodeFailed) {
      closeConnectNode();
    } else {
      closeStatusConnectNode();
    }
    handlerEvent(CLOSE_JSON_STRING, CLOSE_CODE,
                 NlsEvent::Close, _enableOnMessage);
  }

  return Success;
}

int ConnectNode::handlerFrame(NlsEvent *frameEvent) {
  if (_workStatus == NodeInvalid) {
    LOG_ERROR("Node(%p) current node status:%s is invalid, skip callback.",
        this, getConnectNodeStatusString().c_str());
    return -(InvaildNodeStatus);
  } else {
    if (_handler == NULL) {
      LOG_ERROR("Node(%p) _handler is nullptr!", this)
      return -(NlsEventEmpty);
    }

#ifdef ENABLE_REQUEST_RECORDING
    int msg_type = frameEvent->getMsgType();
    switch(msg_type) {
      case NlsEvent::RecognitionStarted:
      case NlsEvent::TranscriptionStarted:
        _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
        _nodeProcess.started_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
        break;
      case NlsEvent::TranscriptionCompleted:
      case NlsEvent::RecognitionCompleted:
      case NlsEvent::SynthesisCompleted:
        _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
        _nodeProcess.completed_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
        break;
      case NlsEvent::Binary:
        _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
        _nodeProcess.play_bytes += frameEvent->getBinaryData().size();
        _nodeProcess.play_count++;
        _nodeProcess.last_status = NodePlayAudio;
        if (_isFirstBinaryFrame) {
          _nodeProcess.first_binary_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
        }
        break;
    }
#endif

    // LOG_DEBUG("Node:%p current node status:%s is valid, msg type:%s handle message ...",
    //     this, getConnectNodeStatusString().c_str(),
    //     frameEvent->getMsgTypeString().c_str());
    // LOG_DEBUG("Node:%p current response:%s.", this, frameEvent->getAllResponse());
    if (_enableOnMessage) {
      sendFinishCondSignal(NlsEvent::Message);
      handlerMessage(frameEvent->getAllResponse(),
                     NlsEvent::Message);
    } else {
      sendFinishCondSignal(frameEvent->getMsgType());
      _handler->handlerFrame(*frameEvent);
    }
  }
  return Success;
}

void ConnectNode::handlerEvent(const char* error,
                               int errorCode,
                               NlsEvent::EventType eventType,
                               bool ignore) {
  LOG_DEBUG("Node(%p) 's exit status:%s, eventType:%d.",
      this, getExitStatusString().c_str(), eventType);
  if (_exitStatus == ExitCancel) {
    LOG_WARN("Node(%p) invoke cancel command, callback won't be invoked.", this);
    return;
  }

  if (_request == NULL) {
    LOG_ERROR("The request of this node(%p) is nullptr.", this);
    return;
  }

  if (errorCode != CLOSE_CODE) {
    _nodeErrCode = errorCode;
  }

  std::string error_str(error);
#ifdef ENABLE_REQUEST_RECORDING
  if (eventType == NlsEvent::Close || eventType == NlsEvent::TaskFailed) {
    if (eventType == NlsEvent::TaskFailed) {
      _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
      _nodeProcess.failed_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
    } else if (eventType == NlsEvent::Close) {
      _nodeProcess.last_op_timestamp_ms = utility::TextUtils::GetTimestampMs();
      _nodeProcess.closed_timestamp_ms = _nodeProcess.last_op_timestamp_ms;
    }
    error_str.assign(replenishNodeProcess(error));
    if (eventType == NlsEvent::TaskFailed) {
      LOG_ERROR("Node(%p) trigger message: %s", this, error_str.c_str());
    } else if (eventType == NlsEvent::Close) {
      LOG_INFO("Node(%p) trigger message: %s", this, error_str.c_str());
    }
  }
#endif
  NlsEvent* useEvent = NULL;
  useEvent = new NlsEvent(
      error_str.c_str(), errorCode, eventType, _request->getRequestParam()->_task_id);
  if (useEvent == NULL) {
    LOG_ERROR("Node(%p) new NlsEvent failed.", this);
    return;
  }

  if (eventType == NlsEvent::Close) {
    LOG_INFO("Node(%p) will callback NlsEvent::Close frame.", this);
  }

  if (_handler == NULL) {
    LOG_ERROR("Node(%p) event type:%d 's _handler is nullptr!", this, eventType)
  } else {
    if (NodeClosed == _workStatus) {
      LOG_WARN("Node(%p) NlsEvent::Close has invoked, skip CloseCallback.", this);
    } else {
      handlerFrame(useEvent);
      if (eventType == NlsEvent::Close) {
        _workStatus = NodeClosed;
        LOG_INFO("Node(%p) callback NlsEvent::Close frame done.", this);
      } else {
        LOG_INFO("Node(%p) callback NlsEvent::%s frame done.", this, useEvent->getMsgTypeString().c_str());
      }
    }
  }

  delete useEvent;
  useEvent = NULL;

  return;
}

void ConnectNode::handlerMessage(const char* response,
                                 NlsEvent::EventType eventType) {
  NlsEvent* useEvent = NULL;
  useEvent = new NlsEvent(
      response, 0, eventType, _request->getRequestParam()->_task_id);
  if (useEvent == NULL) {
    LOG_ERROR("Node(%p) new NlsEvent failed.", this);
    return;
  }

  if (_workStatus == NodeInvalid) {
    LOG_ERROR("Node(%p) node status:%s is invalid, skip callback.",
        this, getConnectNodeStatusString().c_str());
  } else {
    _handler->handlerFrame(*useEvent);
  }

  delete useEvent;
  useEvent = NULL;
  return;
}

int ConnectNode::getErrorCodeFromMsg(const char* msg) {
  int code = DefaultErrorCode;
  Json::Reader reader;
  Json::Value root(Json::objectValue);

  // parse json if existent
  if (reader.parse(msg, root)) {
    if (!root["header"].isNull() && root["header"].isObject()) {
      Json::Value head = root["header"];
      if (!head["status"].isNull() && head["status"].isInt()) {
        code = head["status"].asInt();
      }
    }
  } else {
    if (strstr(msg, "return of SSL_read:")) {
      if (strstr(msg, "error:00000000:lib(0):func(0):reason(0)")||
          strstr(msg, "shutdown while in init")) {
        code = DefaultErrorCode;
      }
    } else if (strstr(msg, "ACCESS_DENIED")) {
      if (strstr(msg, "The token")) {
        if (strstr(msg, "has expired")) {
          code = TokenHasExpired;
        } else if (strstr(msg, "is invalid")) {
          code = TokenIsInvalid;
        }
      } else if (strstr(msg, "No privilege to this voice")) {
        code = NoPrivilegeToVoice;
      } else if (strstr(msg, "Missing authorization header")) {
        code = MissAuthHeader;
      }
    } else if (strstr(msg, "Got bad status")) {
      if (strstr(msg, "403 Forbidden")) {
        code = HttpGotBadStatusWith403;
      }
    } else if (strstr(msg, "Operation now in progress")) {
      code = OpNowInProgress;
    } else if (strstr(msg, "Broken pipe")) {
      code = BrokenPipe;
    } else if (strstr(msg, "connect failed")) {
      code = SysConnectFailed;
    }
  }

  return code;
}

void ConnectNode::handlerTaskFailedEvent(std::string failedInfo, int code) {
  char tmp_msg[1024] = {0};

  if (failedInfo.empty()) {
    strcpy(tmp_msg, "{\"TaskFailed\":\"Unknown failed.\"}");
    code = DefaultErrorCode;
  } else {
    snprintf(tmp_msg, 1024 - 1, "{\"TaskFailed\":\"%s\"}", failedInfo.c_str());
    if (code == DefaultErrorCode) {
      code = getErrorCodeFromMsg(failedInfo.c_str());
    }
  }

  handlerEvent(tmp_msg, code, NlsEvent::TaskFailed, _enableOnMessage);
  handlerEvent(CLOSE_JSON_STRING, CLOSE_CODE, NlsEvent::Close, _enableOnMessage);
  return;
}

#ifndef _MSC_VER
static void *async_dns_resolve_thread_fn(void *arg) {
  pthread_detach(pthread_self());
  ConnectNode *node = (ConnectNode *)arg;
  LOG_DEBUG("Node(%p) dnsThread(%lu) is working ...", node, pthread_self());

  if (node->_gaicbRequest[0] == NULL) {
    node->_gaicbRequest[0] = (struct gaicb *)malloc(sizeof(*node->_gaicbRequest[0]));
  }
  if (node->_gaicbRequest[0] == NULL) {
    LOG_ERROR("Node(%p) malloc _gaicbRequest failed.", arg);
  } else {
    memset(node->_gaicbRequest[0], 0, sizeof(*node->_gaicbRequest[0]));
    node->_gaicbRequest[0]->ar_name = node->_nodename;

    // LOG_DEBUG("Node(%p) ready to getaddrinfo_a ...", node);
    int err = getaddrinfo_a(GAI_NOWAIT, node->_gaicbRequest, 1, NULL);
    if (err) {
      LOG_ERROR("Node(%p) getaddrinfo_a failed, err:%d(%s).", arg, err, gai_strerror(err));
    } else {
      /* check this node is alive. */
      if (node->_request == NULL) {
        LOG_ERROR("The request of this node(%p) is nullptr.", node);
        goto dnsExit;
      }
      NlsClient* instance = node->getInstance();
      NlsNodeManager* node_manager = (NlsNodeManager*)instance->getNodeManger();
      int status = NodeStatusInvalid;
      int result = node_manager->checkNodeExist(node, &status);
      if (result != Success) {
        LOG_ERROR("Node(%p) checkNodeExist failed, result:%d.", node, result);
        goto dnsExit;
      }

      int timeout_ms = node->_request->getRequestParam()->getTimeout();  // ms
      time_t time_s = timeout_ms / 1000;
      time_t time_ns = (timeout_ms % 1000) * 1000000;
      struct timespec outtime = { time_s, time_ns };
      // LOG_DEBUG("Node(%p) ready to gai_suspend ...", node);
      err = gai_suspend(node->_gaicbRequest, 1, &outtime);
      // LOG_DEBUG("Node(%p) gai_suspend finish, err:%d(%s).", node, err, gai_strerror(err));
      if (node->_dnsThreadExit) {
        LOG_WARN("Node(%p) ConnectNode is exiting, skip gai.", node);
        goto dnsExit;
      }
      if (node->_dnsThread == 0) {
        LOG_WARN("Node(%p) ConnectNode has exited, skip gai.", node);
        goto dnsExit;
      }
      if (err) {
        LOG_ERROR("Node(%p) gai_suspend failed, err:%d(%s).", arg, err, gai_strerror(err));
        if (err == EAI_SYSTEM || err == EAI_AGAIN) {
          LOG_ERROR("Node(%p) gai_suspend err:%d is mean timeout. please try again ...", arg, err);
        }
      } else {
        /* check this node is alive again after gai_suspend. */
        result = node_manager->checkNodeExist(node, &status);
        if (result != Success) {
          LOG_ERROR("Node(%p) checkNodeExist failed, result:%d.", node, result);
          goto dnsExit;
        }
        
        err = gai_error(node->_gaicbRequest[0]);
        if (err) {
          LOG_WARN("Node(%p) cannot get addrinfo, err:%d(%s).", arg, err, gai_strerror(err));
        } else {
          node->_addrinfo = node->_gaicbRequest[0]->ar_result;
        }
      }
    }

    node->_dnsErrorCode = err;
    // LOG_DEBUG("Node(%p) async_dns_resolve_thread_fn event_assign ->", node);
    if (node->_dnsEvent) {
      event_del(node->_dnsEvent);
      event_assign(node->_dnsEvent, node->_eventThread->_workBase,
                   -1, EV_READ, WorkThread::sysDnsEventCallback, node);
    } else {
      node->_dnsEvent = event_new(node->_eventThread->_workBase,
                                  -1, EV_READ, WorkThread::sysDnsEventCallback, node);
      if (NULL == node->_dnsEvent) {
        LOG_ERROR("Node(%p) new event(_dnsEvent) failed.", node);
        goto dnsExit;
      }
    }
    // LOG_DEBUG("Node(%p) async_dns_resolve_thread_fn event_add ->", node);
    event_add(node->_dnsEvent, NULL);
    // LOG_DEBUG("Node(%p) async_dns_resolve_thread_fn event_active ->", node);
    event_active(node->_dnsEvent, EV_READ, 0);
    LOG_INFO("Node(%p) dnsThread(%lu) event_active done.", node, pthread_self());
  }

dnsExit:
  if (node->_gaicbRequest[0]) {
    free(node->_gaicbRequest[0]);
    node->_gaicbRequest[0] = NULL;
  }
  node->_dnsThread = 0;
  node->_dnsThreadRunning = false;
  LOG_INFO("Node(%p) dnsThread(%lu) is exited.", node, pthread_self());
  pthread_exit(NULL);
}

/**
 * @brief: 异步方式使用系统的getaddrinfo()方法获得dns
 * @return: 成功则为0, 否则为失败
 */
static int native_getaddrinfo(
    const char *nodename, const char *servname,
    evdns_getaddrinfo_cb cb, void *arg) {
  int err = 0;
  ConnectNode *node = (ConnectNode *)arg;

#if defined(_MSC_VER)
  WaitForSingleObject(node->_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&node->_mtxNode);
#endif

  NlsNodeManager* node_manager = (NlsNodeManager*)node->getInstance()->getNodeManger();
  int status = NodeStatusInvalid;
  int result = node_manager->checkNodeExist(node, &status);
  if (result != Success) {
    LOG_ERROR("Node(%p) checkNodeExist failed, result:%d.", node, result);
    #if defined(_MSC_VER)
    ReleaseMutex(node->_mtxNode);
    #else
    pthread_mutex_unlock(&node->_mtxNode);
    #endif
    return result;
  } else {
    LOG_DEBUG("Node(%p) use native_getaddrinfo, ready to create dnsThread(%lu).", node, node->_dnsThread);
  }

  if (node->_exitStatus == ExitCancel) {
    LOG_WARN("Node(%p) is ExitCancel, skip here ...", node);
    #if defined(_MSC_VER)
    ReleaseMutex(node->_mtxNode);
    #else
    pthread_mutex_unlock(&node->_mtxNode);
    #endif
    return -(CancelledExitStatus);
  }

  node->_dnsThreadRunning = true;
  node->_nodename = (char*)nodename;
  node->_servname = (char*)servname;

  if (node->_dnsThread) {
    pthread_join(node->_dnsThread, NULL);
  }
  err = pthread_create(&node->_dnsThread, NULL,
                       &async_dns_resolve_thread_fn, arg);
  if (err) {
    node->_dnsThreadRunning = false;
    LOG_ERROR("Node(%p) dnsThread(%lu) create failed.", node, node->_dnsThread);
  } else {
    LOG_DEBUG("Node(%p) dnsThread(%lu) create success.", node, node->_dnsThread);
  }

#if defined(_MSC_VER)
  ReleaseMutex(node->_mtxNode);
#else
  pthread_mutex_unlock(&node->_mtxNode);
#endif

  return err;
}
#endif

/**
 * @brief: 进行DNS解析
 * @return: 成功则为正值, 失败则为负值
 */
int ConnectNode::dnsProcess(int aiFamily, char *directIp, bool sysGetAddr) {
  EXIT_CANCEL_CHECK(_exitStatus, this);
  int result = Success;
  struct evutil_addrinfo hints;
  NlsNodeManager* node_manager = (NlsNodeManager*)_instance->getNodeManger();
  int status = NodeStatusInvalid;
  result = node_manager->checkNodeExist(this, &status);
  if (result != Success) {
    LOG_ERROR("Node(%p) checkNodeExist failed, result:%d.", this, result);
    return result;
  }

  /* 当node处于长链接模式且已经链接, 无需进入握手阶段, 直接进入starting阶段. */
  if (_isLongConnection && _isConnected) {
    LOG_DEBUG("Node(%p) has connected, current is longConnection and connected.", this);
    _workStatus = NodeStarting;
    node_manager->updateNodeStatus(this, NodeStatusRunning);
    if (_request->getRequestParam()->_requestType == SpeechTextDialog) {
      addCmdDataBuffer(CmdTextDialog);
    } else {
      addCmdDataBuffer(CmdStart);
    }
    result = nlsSendFrame(getCmdEvBuffer());
    if (result < 0) {
      LOG_ERROR("Node(%p) response failed, result:%d.", this, result);
      handlerTaskFailedEvent(getErrorMsg());
      closeConnectNode();
      return result;
    }
    return Success;
  }

  /* 尝试链接校验 */
  if (!checkConnectCount()) {
    LOG_ERROR("Node(%p) restart connect failed.", this);
    handlerTaskFailedEvent(
        TASKFAILED_CONNECT_JSON_STRING, SysConnectFailed);
    return -(ConnectFailed);
  }

  _workStatus = NodeConnecting;
  node_manager->updateNodeStatus(this, NodeStatusConnecting);

  if (!parseUrlInformation(directIp)) {
    return -(ParseUrlFailed);
  }

  _url._enableSysGetAddr = sysGetAddr;
  if (_url._isSsl) {
    LOG_INFO("Node(%p) _url._isSsl is True, _url._enableSysGetAddr is %d.", this, _url._enableSysGetAddr);
  } else {
    LOG_INFO("Node(%p) _url._isSsl is False, _url._enableSysGetAddr is %d.", this, _url._enableSysGetAddr);
  }

  if (_url._directIp) {
    LOG_INFO("Node(%p) _url._directIp is True.", this);
    WorkThread::directConnect(this, _url._address);
  } else {
    LOG_INFO("Node(%p) _url._directIp is False.", this);

    if (aiFamily != AF_UNSPEC && aiFamily != AF_INET && aiFamily != AF_INET6) {
      LOG_WARN("Node(%p) aiFamily is invalid, use default AF_INET.",
          this, aiFamily);
      aiFamily = AF_INET;
    }

    memset(&hints,  0,  sizeof(hints));
    hints.ai_family = aiFamily;
    hints.ai_flags = EVUTIL_AI_CANONNAME;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    LOG_INFO("Node(%p) dns url:%s, enableSysGetAddr:%d.",
        this, _request->getRequestParam()->_url.c_str(),
        _url._enableSysGetAddr);

    if (_url._enableSysGetAddr) {
    #ifndef _MSC_VER
      /*
       * 在内部ws协议下或者主动使用系统getaddrinfo的情况下, 使用系统的getaddrinfo
       */
      result = native_getaddrinfo(_url._host,
                                  NULL,
                                  WorkThread::dnsEventCallback,
                                  this);
      if (result != Success) {
        result = -(GetAddrinfoFailed);
      }
    #else
      if (NULL == _eventThread || NULL == _eventThread->_dnsBase) {
        LOG_ERROR("Node:%p dns source is invalid.", this);
        return -(InvalidDnsSource);
      }
      _dnsRequest = evdns_getaddrinfo(_eventThread->_dnsBase,
                                      _url._host,
                                      NULL,
                                      &hints,
                                      WorkThread::dnsEventCallback,
                                      this);
    #endif
    } else {
      if (NULL == _eventThread || NULL == _eventThread->_dnsBase) {
        LOG_ERROR("Node(%p) dns source is invalid.", this);
        return -(InvalidDnsSource);
      }
      _dnsRequest = evdns_getaddrinfo(_eventThread->_dnsBase,
                                      _url._host,
                                      NULL,
                                      &hints,
                                      WorkThread::dnsEventCallback,
                                      this);
      if (_dnsRequest == NULL) {
        LOG_ERROR("Node:%p dnsRequest evdns_getaddrinfo failed!", this);
        /*
         * No need to free user_data ordecrement n_pending_requests; that
         * happened in the callback.
         */
        return -(InvalidDnsSource);
      }
    }
  }

  return result;
}

/**
 * @brief: 开始进行链接处理, 创建socket, 设置libevent, 并开始socket链接.
 * @return: 成功则为0, 阻塞则为1, 失败则负值.
 */
int ConnectNode::connectProcess(const char *ip, int aiFamily) {
  EXIT_CANCEL_CHECK(_exitStatus, this);
  evutil_socket_t sockFd = socket(aiFamily, SOCK_STREAM, 0);
  if (sockFd < 0) {
    LOG_ERROR("Node(%p) socket failed. aiFamily:%d, sockFd:%d. error mesg:%s.",
        this, aiFamily, sockFd,
        evutil_socket_error_to_string(evutil_socket_geterror(sockFd)));
    return -(SocketFailed);
  } else {
    // LOG_DEBUG("Node(%p) socket success, sockFd:%d.", this, sockFd);
  }

  struct linger so_linger;
  so_linger.l_onoff = 1;
  so_linger.l_linger = 0;
  if (setsockopt(sockFd, SOL_SOCKET, SO_LINGER,
      (char *)&so_linger, sizeof(struct linger)) < 0) {
    LOG_ERROR("Node(%p) set SO_LINGER failed.", this);
    return -(SetSocketoptFailed);
  }

  if (evutil_make_socket_nonblocking(sockFd) < 0) {
    LOG_ERROR("Node(%p) evutil_make_socket_nonblocking failed.", this);
    return -(EvutilSocketFalied);
  }

  LOG_INFO("Node(%p) new socket ip:%s port:%d Fd:%d.",
      this, ip, _url._port, sockFd);

  short events = EV_READ | EV_WRITE | EV_TIMEOUT | EV_FINALIZE;
  // LOG_DEBUG("Node(%p) set events(%d) for connectEventCallback.", this, events);
  if (NULL == _connectEvent) {
    _connectEvent = event_new(_eventThread->_workBase, sockFd,
                              events,
                              WorkThread::connectEventCallback,
                              this);
    if (NULL == _connectEvent) {
      LOG_ERROR("Node(%p) new event(_connectEvent) failed.", this);
      return -(EventEmpty);
    }
  } else {
    event_del(_connectEvent);
    event_assign(_connectEvent, _eventThread->_workBase, sockFd,
                 events,
                 WorkThread::connectEventCallback,
                 this);
  }

#ifdef ENABLE_HIGH_EFFICIENCY
  if (NULL == _connectTimerEvent) {
    _connectTimerEvent = evtimer_new(_eventThread->_workBase,
                                     WorkThread::connectTimerEventCallback,
                                     this);
    if (NULL == _connectTimerEvent) {
      LOG_ERROR("Node(%p) new event(_connectTimerEvent) failed.", this);
      return -(EventEmpty);
    }
  }
#endif

  if (_enableRecvTv) {
    events = EV_READ | EV_TIMEOUT | EV_PERSIST | EV_FINALIZE;
  } else {
    events = EV_READ | EV_PERSIST | EV_FINALIZE;
  }
  // LOG_DEBUG("Node(%p) set events(%d) for readEventCallback", this, events);
  if (NULL == _readEvent) {
    _readEvent = event_new(_eventThread->_workBase, sockFd,
                           events,
                           WorkThread::readEventCallBack,
                           this);
    if (NULL == _readEvent) {
      LOG_ERROR("Node(%p) new event(_readEvent) failed.", this);
      return -(EventEmpty);
    }
  } else {
    event_del(_readEvent);
    event_assign(_readEvent, _eventThread->_workBase, sockFd,
                 events,
                 WorkThread::readEventCallBack,
                 this);
  }

  events = EV_WRITE | EV_TIMEOUT | EV_FINALIZE;
  // LOG_DEBUG("Node(%p) set events(%d) for writeEventCallback", this, events);
  if (NULL == _writeEvent) {
    _writeEvent = event_new(_eventThread->_workBase, sockFd,
                            events,
                            WorkThread::writeEventCallBack,
                            this);
    if (NULL == _writeEvent) {
      LOG_ERROR("Node(%p) new event(_writeEvent) failed.", this);
      return -(EventEmpty);
    }
  } else {
    event_del(_writeEvent);
    event_assign(_writeEvent, _eventThread->_workBase, sockFd,
                 events,
                 WorkThread::writeEventCallBack,
                 this);
  }

  _aiFamily = aiFamily;
  if (aiFamily == AF_INET) {
    memset(&_addrV4, 0, sizeof(_addrV4));
    _addrV4.sin_family = AF_INET;
    _addrV4.sin_port = htons(_url._port);

    if (inet_pton(AF_INET, ip, &_addrV4.sin_addr) <= 0) {
      LOG_ERROR("Node(%p) IpV4 inet_pton failed.", this);
      evutil_closesocket(sockFd);
      return -(SetSocketoptFailed);
    }
  } else if (aiFamily == AF_INET6) {
    memset(&_addrV6, 0, sizeof(_addrV6));
    _addrV6.sin6_family = AF_INET6;
    _addrV6.sin6_port = htons(_url._port);

    if (inet_pton(AF_INET6, ip, &_addrV6.sin6_addr) <= 0) {
      LOG_ERROR("Node(%p) IpV6 inet_pton failed.", this);
      evutil_closesocket(sockFd);
      return -(SetSocketoptFailed);
    }
  }

  _socketFd = sockFd;

  return socketConnect();
}

/**
 * @brief:进行socket链接
 * @return: 成功则为0, 阻塞则为1, 失败则负值.
 */
int ConnectNode::socketConnect() {
  int retCode = 0;
  int connectErrCode = 0;
  NlsClient* client = _instance;
  NlsNodeManager* node_manager = (NlsNodeManager*)client->getNodeManger();
  int status = NodeStatusInvalid;
  int ret = node_manager->checkNodeExist(this, &status);
  if (ret != Success) {
    LOG_ERROR("Node(%p) checkNodeExist failed, ret:%d.", this, ret);
    return ret;
  }

  if (ExitCancel == _exitStatus) {
    LOG_WARN("Node(%p) current ExitCancel, skip sslProcess.", this);
    return -(InvalidExitStatus);
  }

  if (_aiFamily == AF_INET) {
    retCode = connect(_socketFd, (const sockaddr *)&_addrV4, sizeof(_addrV4));
  } else {
    retCode = connect(_socketFd, (const sockaddr *)&_addrV6, sizeof(_addrV6));
  }

  if (retCode == -1) {
    connectErrCode = utility::getLastErrorCode();
    if (NLS_ERR_CONNECT_RETRIABLE(connectErrCode)) {
      /* 
       *  connectErrCode == 115(EINPROGRESS)
       *  means connection is in progress,
       *  normally the socket connecting timeout is 75s.
       *  after the socket fd is ready to read.
       */

      /* 开启用于链接的libevent */
      if (NULL == _connectEvent) {
        LOG_ERROR("Node(%p) event is nullptr.", this);
        return -(EventEmpty);
      }

      int timeout_ms = _request->getRequestParam()->getTimeout();
      _connectTv.tv_sec = timeout_ms / 1000;
      _connectTv.tv_usec = (timeout_ms - _connectTv.tv_sec * 1000) * 1000;
      event_add(_connectEvent, &_connectTv);

      LOG_DEBUG("Node(%p) will connect later, errno:%d. timeout:%dms.",
          this, connectErrCode, timeout_ms);
      return 1;
    } else {
      LOG_ERROR("Node(%p) connect failed:%s. retCode:%d connectErrCode:%d. retry ...",
          this, evutil_socket_error_to_string(evutil_socket_geterror(_socketFd)),
          retCode, connectErrCode);
      return -(SocketConnectFailed);
    }
  } else {
    LOG_DEBUG("Node(%p) connected directly. retCode:%d.", this, retCode);
    _workStatus = NodeConnected;
    node_manager->updateNodeStatus(this, NodeStatusConnected);
    _isConnected = true;
  }
  return 0;
}

/**
 * @brief: 进行SSL握手
 * @return: 成功 等待链接则返回1, 正在握手则返回0, 失败则返回负值
 */
int ConnectNode::sslProcess() {
  EXIT_CANCEL_CHECK(_exitStatus, this);
  int ret = 0;
  NlsClient* client = _instance;
  NlsNodeManager* node_manager = (NlsNodeManager*)client->getNodeManger();
  int status = NodeStatusInvalid;
  ret = node_manager->checkNodeExist(this, &status);
  if (ret != Success) {
    LOG_ERROR("Node(%p) checkNodeExist failed, ret:%d", this, ret);
    return ret;
  }

  if (_url._isSsl) {
    ret = _sslHandle->sslHandshake(_socketFd, _url._host);
    if (ret == SSL_ERROR_WANT_READ || ret == SSL_ERROR_WANT_WRITE) {
      // current status:NodeConnected
      // LOG_DEBUG("Node(%p) status:%s, wait ssl process ...",
      //     this, getConnectNodeStatusString().c_str());

      // trigger connectEventCallback()
      #ifdef ENABLE_HIGH_EFFICIENCY
      // connect回调和定时connect回调交替调用, 降低事件量的同时保证稳定
      if (!_connectTimerFlag) {
        if (NULL == _connectTimerEvent) {
          LOG_ERROR("Node(%p) event is nullptr.", this);
          return -(EventEmpty);
        }

        // LOG_DEBUG("Node(%p) add evtimer _connectTimerEvent.", this);
        evtimer_add(_connectTimerEvent, &_connectTimerTv);
        _connectTimerFlag = true;
      } else {
        if (NULL == _connectEvent) {
          LOG_ERROR("Node(%p) event is nullptr.", this);
          return -(EventEmpty);
        }

        // LOG_DEBUG("Node(%p) add events _connectEvent.", this);
        int timeout_ms = _request->getRequestParam()->getTimeout();
        _connectTv.tv_sec = timeout_ms / 1000;
        _connectTv.tv_usec = (timeout_ms - _connectTv.tv_sec * 1000) * 1000;
        // set _connectEvent to pending status.
        event_add(_connectEvent, &_connectTv);
        _connectTimerFlag = false;
      }
      #else
      if (NULL == _connectEvent) {
        LOG_ERROR("Node(%p) event is nullptr.", this);
        return -(EventEmpty);
      }
      int timeout_ms = _request->getRequestParam()->getTimeout();
      _connectTv.tv_sec = timeout_ms / 1000;
      _connectTv.tv_usec = (timeout_ms - _connectTv.tv_sec * 1000) * 1000;
      event_add(_connectEvent, &_connectTv);
      #endif

      return 1;
    } else if (ret < 0) {
      _nodeErrMsg = _sslHandle->getFailedMsg();
      LOG_ERROR("Node(%p) sslHandshake failed, %s.", this, _nodeErrMsg.c_str());
      return -(SslHandshakeFailed);
    } else {
      _workStatus = NodeHandshaking;
      node_manager->updateNodeStatus(this, NodeStatusHandshaking);
      LOG_DEBUG("Node(%p) sslHandshake done, ret:%d, set node:NodeHandshaking.", this, ret);
      return 0;
    }
  } else {
    _workStatus = NodeHandshaking;
    node_manager->updateNodeStatus(this, NodeStatusHandshaking);
    LOG_INFO("Node(%p) it's not ssl process, set node:NodeHandshaking.", this);
  }

  return 0;
}

/**
 * @brief: 初始化音频编码器
 * @return:
 */
void ConnectNode::initNlsEncoder() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  if (NULL == _nlsEncoder) {
    if (NULL == _request) {
      LOG_ERROR("The request of node(%p) is nullptr.", this);
    #if defined(_MSC_VER)
      ReleaseMutex(_mtxNode);
    #else
      pthread_mutex_unlock(&_mtxNode);
    #endif
      return;
    }

    if (_request->getRequestParam()->_format == "opu") {
      _encoderType = ENCODER_OPU;
    } else if (_request->getRequestParam()->_format == "opus") {
      _encoderType = ENCODER_OPUS;
    }

    if (_encoderType != ENCODER_NONE) {
      _nlsEncoder = new NlsEncoder();
      if (NULL == _nlsEncoder) {
        LOG_ERROR("Node(%p) new _nlsEncoder failed.", this);
        #if defined(_MSC_VER)
        ReleaseMutex(_mtxNode);
        #else
        pthread_mutex_unlock(&_mtxNode);
        #endif
        return;
      }

      int errorCode = 0;
      int ret = _nlsEncoder->createNlsEncoder(
          _encoderType, 1,
          _request->getRequestParam()->_sampleRate,
          &errorCode);
      if (ret < 0) {
        LOG_ERROR("Node(%p) createNlsEncoder failed, errcode:%d.", this, errorCode);
        delete _nlsEncoder;
        _nlsEncoder = NULL;
      }
    }
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

/**
 * @brief: 更新并生效所有ConnectNode中设置的参数
 * @return:
 */
void ConnectNode::updateParameters() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxNode, INFINITE);
#else
  pthread_mutex_lock(&_mtxNode);
#endif

  if (_request->getRequestParam()->_sampleRate == SAMPLE_RATE_16K) {
    _limitSize = BUFFER_16K_MAX_LIMIT;
  } else {
    _limitSize = BUFFER_8K_MAX_LIMIT;
  }

  int timeout_ms = _request->getRequestParam()->getTimeout();
  _connectTv.tv_sec = timeout_ms / 1000;
  _connectTv.tv_usec = (timeout_ms - _connectTv.tv_sec * 1000) * 1000;
  LOG_INFO("Node(%p) set connect timeout: %dms.", this, timeout_ms);

  _enableRecvTv = _request->getRequestParam()->getEnableRecvTimeout();
  if (_enableRecvTv) {
    LOG_INFO("Node(%p) enable recv timeout.", this);
  } else {
    LOG_INFO("Node(%p) disable recv timeout.", this);
  }
  timeout_ms = _request->getRequestParam()->getRecvTimeout();
  _recvTv.tv_sec = timeout_ms / 1000;
  _recvTv.tv_usec = (timeout_ms - _recvTv.tv_sec * 1000) * 1000;
  LOG_INFO("Node(%p) set recv timeout: %dms.", this, timeout_ms);

  timeout_ms = _request->getRequestParam()->getSendTimeout();
  _sendTv.tv_sec = timeout_ms / 1000;
  _sendTv.tv_usec = (timeout_ms - _sendTv.tv_sec * 1000) * 1000;
  LOG_INFO("Node(%p) set send timeout: %dms.", this, timeout_ms);

  _enableOnMessage = _request->getRequestParam()->getEnableOnMessage();
  if (_enableOnMessage) {
    LOG_INFO("Node(%p) enable OnMessage Callback.", this);
  } else {
    LOG_INFO("Node(%p) disable OnMessage Callback.", this);
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxNode);
#else
  pthread_mutex_unlock(&_mtxNode);
#endif
}

/**
 * @brief: 等待所有EventCallback退出并删除和停止EventCallback的队列
 * @return:
 */
void ConnectNode::delAllEvents() {
  LOG_DEBUG("Node(%p) delete all events begin, current node status:%s exit status:%s.",
      this,
      getConnectNodeStatusString().c_str(),
      getExitStatusString().c_str());

  /* 当Node处于Invoking状态, 即还未进入正式运行, 需要等待其进入Invoked才可进行操作 */
  int try_count = 2000;
  while (_workStatus == NodeInvoking && try_count-- > 0) {
  #ifdef _MSC_VER
    Sleep(1);
  #else
    usleep(1000);
  #endif
  }
  if (try_count <=0) {
    LOG_WARN("Node(%p) waiting exit NodeInvoking failed.", this);
  } else {
    LOG_WARN("Node(%p) waiting exit NodeInvoking success.", this);
  }

  /* 当Node处于异步dns线程状态, 通知线程退出并等待其退出再进行释放 */
  if (_url._enableSysGetAddr && _dnsThreadRunning) {
    _dnsThreadExit = true;
    try_count = 5000;
    LOG_WARN("Node(%p) waiting exit dnsThread ...", this);
    while (_dnsThreadRunning && try_count-- > 0) {
    #ifdef _MSC_VER
      Sleep(1);
    #else
      usleep(1000);
    #endif
    }
    if (try_count <=0) {
      LOG_WARN("Node(%p) waiting exit dnsThread failed.", this);
    } else {
      LOG_WARN("Node(%p) waiting exit dnsThread success.", this);
    }
  }

  waitEventCallback();

  if (_dnsEvent) {
    event_del(_dnsEvent);
    event_free(_dnsEvent);
    _dnsEvent = NULL;
  }
  if (_readEvent) {
    event_del(_readEvent);
    event_free(_readEvent);
    _readEvent = NULL;
  }
  if (_writeEvent) {
    event_del(_writeEvent);
    event_free(_writeEvent);
    _writeEvent = NULL;
  }
  if (_connectEvent) {
    event_del(_connectEvent);
    event_free(_connectEvent);
    _connectEvent = NULL;
  }
#ifdef ENABLE_HIGH_EFFICIENCY
  if (_connectTimerEvent != NULL) {
    if (_connectTimerFlag) {
      evtimer_del(_connectTimerEvent);
      _connectTimerFlag = false;
    }
    event_free(_connectTimerEvent);
    _connectTimerEvent = NULL;
  }
#endif

  waitEventCallback();

  LOG_DEBUG("Node(%p) delete all events done.", this);
}

/**
 * @brief: 等待所有EventCallback退出, 默认1s超时.
 * @return:
 */
void ConnectNode::waitEventCallback() {
  // LOG_DEBUG("Node:%p waiting EventCallback, current node status:%s exit status:%s",
  //     this,
  //     getConnectNodeStatusString().c_str(),
  //     getExitStatusString().c_str());

  if (_inEventCallbackNode) {
    LOG_DEBUG("Node(%p) waiting EventCallback ...", this);

  #if defined(_MSC_VER)
    WaitForSingleObject(_mtxEventCallbackNode, INFINITE);
  #else
    struct timespec outtime;
    struct timeval now;
    gettimeofday(&now, NULL);
    outtime.tv_sec = now.tv_sec + 1;
    outtime.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&_mtxEventCallbackNode);
    if (ETIMEDOUT == pthread_cond_timedwait(&_cvEventCallbackNode, &_mtxEventCallbackNode, &outtime)) {
      LOG_WARN("Node(%p) waiting EventCallback timeout.", this);
    }
    pthread_mutex_unlock(&_mtxEventCallbackNode);
  #endif
    LOG_DEBUG("Node(%p) waiting EventCallback done.", this);
  }

  // LOG_DEBUG("Node(%p) wait all EventCallback exit done.", this);
}

/**
 * @brief: 等待调用完成, 默认10s超时.
 * @return:
 */
void ConnectNode::waitInvokeFinish() {
  if (_syncCallTimeoutMs > 0) {
    LOG_DEBUG("Node(%p) waiting invoke finish, timeout %dms...", this, _syncCallTimeoutMs);

#if defined(_MSC_VER)
    WaitForSingleObject(_mtxInvokeSyncCallNode, INFINITE);
#else
    struct timespec outtime;
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t time_ms = now.tv_sec * 1000 + now.tv_usec / 1000 + _syncCallTimeoutMs;
    outtime.tv_sec = time_ms / 1000;
    outtime.tv_nsec = (time_ms % 1000) * 1000000;
    pthread_mutex_lock(&_mtxInvokeSyncCallNode);
    if (ETIMEDOUT == pthread_cond_timedwait(&_cvInvokeSyncCallNode, &_mtxInvokeSyncCallNode, &outtime)) {
      _nodeErrCode = InvokeTimeout;
      LOG_WARN("Node(%p) waiting invoke timeout.", this);
    }
    pthread_mutex_unlock(&_mtxInvokeSyncCallNode);
#endif

    LOG_DEBUG("Node(%p) waiting invoke done.", this);
  }
}

/**
 * @brief: 发送调用完成的信号.
 * @return:
 */
void ConnectNode::sendFinishCondSignal(NlsEvent::EventType eventType) {
  if (_syncCallTimeoutMs > 0) {
    if (eventType == NlsEvent::Close ||
        eventType == NlsEvent::RecognitionStarted ||
        eventType == NlsEvent::TranscriptionStarted ||
        eventType == NlsEvent::SynthesisStarted) {
      LOG_DEBUG("Node(%p) send finish cond signal, event type:%d.", this, eventType);

      bool useless_flag = false;
    #ifdef _MSC_VER
      SET_EVENT(useless_flag, _mtxInvokeSyncCallNode);
    #else
      SEND_COND_SIGNAL(_mtxInvokeSyncCallNode, _cvInvokeSyncCallNode, useless_flag);
    #endif
    }
  }
}

std::string ConnectNode::getRandomUuid() {
  char uuidBuff[48] = {0};
#ifdef _MSC_VER
  char* data = NULL;
  UUID uuidhandle;
  RPC_STATUS ret_val = UuidCreate(&uuidhandle);
  if (ret_val != RPC_S_OK) {
    LOG_ERROR("UuidCreate failed");
    return uuidBuff;
  }
  UuidToString(&uuidhandle, (RPC_CSTR*)&data);
  if (data == NULL) {
    LOG_ERROR("UuidToString data is nullptr");
    return uuidBuff;
  }
  int len = strnlen(data, 36);
  int i = 0, j = 0;
  for (i = 0; i < len; i++) {
    if (data[i] != '-') {
      uuidBuff[j++] = data[i];
    }
  }
  RpcStringFree((RPC_CSTR*)&data);
#else
  char tmp[48] = {0};
  uuid_t uuid;
  uuid_generate(uuid);
  uuid_unparse(uuid, tmp);
  int i = 0, j = 0;
  while (tmp[i]) {
    if (tmp[i] != '-') {
      uuidBuff[j++] = tmp[i];
    }
    i++;
  }
#endif
  return uuidBuff;
}

#ifdef ENABLE_REQUEST_RECORDING
std::string ConnectNode::replenishNodeProcess(const char *error) {
  if (error == NULL) {
    return "";
  }

  Json::Reader reader;
  Json::Value root(Json::objectValue);
  if (!reader.parse(error, root)) {
    return "";
  } else {
    Json::FastWriter writer;
    Json::Value request_process(Json::objectValue);
    Json::Value timestamp(Json::objectValue);
    Json::Value last(Json::objectValue);
    Json::Value data(Json::objectValue);
    timestamp["create"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.create_timestamp_ms);
    if (_nodeProcess.start_timestamp_ms > 0) {
      timestamp["start"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.start_timestamp_ms);
    }
    if (_nodeProcess.started_timestamp_ms > 0) {
      timestamp["started"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.started_timestamp_ms);
    }
    if (_nodeProcess.stop_timestamp_ms > 0) {
      timestamp["stop"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.stop_timestamp_ms);
    }
    if (_nodeProcess.cancel_timestamp_ms > 0) {
      timestamp["cancel"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.cancel_timestamp_ms);
    }
    if (_nodeProcess.failed_timestamp_ms > 0) {
      timestamp["failed"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.failed_timestamp_ms);
    }
    if (_nodeProcess.completed_timestamp_ms > 0) {
      timestamp["completed"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.completed_timestamp_ms);
    }
    if (_nodeProcess.closed_timestamp_ms > 0) {
      timestamp["closed"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.closed_timestamp_ms);
    }
    if (_nodeProcess.first_binary_timestamp_ms > 0) {
      timestamp["first_binary"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.first_binary_timestamp_ms);
    }

    last["status"] = getConnectNodeStatusString(_nodeProcess.last_status);
    if (_nodeProcess.last_send_timestamp_ms > 0) {
      last["send"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.last_send_timestamp_ms);
    }
    if (_nodeProcess.last_ctrl_timestamp_ms > 0) {
      last["control"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.last_ctrl_timestamp_ms);
    }
    if (_nodeProcess.last_op_timestamp_ms > 0) {
      last["action"] = utility::TextUtils::GetTimeFromMs(_nodeProcess.last_op_timestamp_ms);
    }

    if (_nodeProcess.recording_bytes > 0) {
      data["recording_bytes"] = (Json::UInt64)_nodeProcess.recording_bytes;
    }
    if (_nodeProcess.send_count > 0) {
      data["send_count"] = (Json::UInt64)_nodeProcess.send_count;
    }
    if (_nodeProcess.play_bytes > 0) {
      data["play_bytes"] = (Json::UInt64)_nodeProcess.play_bytes;
    }
    if (_nodeProcess.play_count > 0) {
      data["play_count"] = (Json::UInt64)_nodeProcess.play_count;
    }

    root["timestamp"] = timestamp;
    root["last"] = last;
    root["data"] = data;
    return writer.write(root);
  }
}
#endif

}  // namespace AlibabaNls
