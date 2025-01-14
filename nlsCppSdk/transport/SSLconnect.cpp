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

#include <stdio.h>
#include <string.h>
#include "openssl/err.h"

#include "nlsGlobal.h"
#include "SSLconnect.h"
#include "connectNode.h"
#include "nlog.h"
#include "utility.h"

namespace AlibabaNls {

SSL_CTX* SSLconnect::_sslCtx = NULL;

SSLconnect::SSLconnect() {
  _ssl = NULL;
  _sslTryAgain = 0;
#if defined(_MSC_VER)
  _mtxSSL = CreateMutex(NULL, FALSE, NULL);
#else
  pthread_mutex_init(&_mtxSSL, NULL);
#endif
  LOG_DEBUG("Create SSLconnect:%p.", this);
}

SSLconnect::~SSLconnect() {
  sslClose();
  _sslTryAgain = 0;
#if defined(_MSC_VER)
  CloseHandle(_mtxSSL);
#else
  pthread_mutex_destroy(&_mtxSSL);
#endif
  LOG_DEBUG("SSL(%p) Destroy SSLconnect done.", this);
}

int SSLconnect::init() {
  if (_sslCtx == NULL) {
    _sslCtx = SSL_CTX_new(SSLv23_client_method());
    if (_sslCtx == NULL) {
      LOG_ERROR("SSL: couldn't create a context!");
      exit(1);
    }
  }

  SSL_CTX_set_verify(_sslCtx, SSL_VERIFY_NONE, NULL);

  SSL_CTX_set_mode(_sslCtx,
      SSL_MODE_ENABLE_PARTIAL_WRITE |
      SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
      SSL_MODE_AUTO_RETRY);

  LOG_DEBUG("SSLconnect::init() done.");
  return Success;
}

void SSLconnect::destroy() {
  if (_sslCtx) {
    // LOG_DEBUG("free _sslCtx.");
    SSL_CTX_free(_sslCtx);
    _sslCtx = NULL;
  }

  LOG_DEBUG("SSLconnect::destroy() done.");
}

int SSLconnect::sslHandshake(int socketFd, const char* hostname) {
  // LOG_DEBUG("Begin sslHandshake.");
  if (_sslCtx == NULL) {
    LOG_ERROR("SSL(%p) _sslCtx has been released.", this);
    return -(SslCtxEmpty);
  }

#if defined(_MSC_VER)
  WaitForSingleObject(_mtxSSL, INFINITE);
#else
  pthread_mutex_lock(&_mtxSSL);
#endif

  int ret;
  if (_ssl == NULL) {
    _ssl = SSL_new(_sslCtx);
    if (_ssl == NULL) {
      memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
      const char *SSL_new_ret = "return of SSL_new: ";
      memcpy(_errorMsg, SSL_new_ret, strnlen(SSL_new_ret, 24));
      ERR_error_string_n(ERR_get_error(),
                         _errorMsg + strnlen(SSL_new_ret, 24),
                         MAX_SSL_ERROR_LENGTH);
      LOG_ERROR("SSL(%p) Invoke SSL_new failed:%s.", this, _errorMsg);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return -(SslNewFailed);
    }

    ret = SSL_set_fd(_ssl, socketFd);
    if (ret == 0) {
      memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
      const char *SSL_set_fd_ret = "return of SSL_set_fd: ";
      memcpy(_errorMsg, SSL_set_fd_ret, strnlen(SSL_set_fd_ret, 24));
      ERR_error_string_n(ERR_get_error(),
                         _errorMsg + strnlen(SSL_set_fd_ret, 24),
                         MAX_SSL_ERROR_LENGTH);
      LOG_ERROR("SSL(%p) Invoke SSL_set_fd failed:%s.", this, _errorMsg);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return -(SslSetFailed);
    }

    SSL_set_mode(_ssl,
        SSL_MODE_ENABLE_PARTIAL_WRITE |
        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
        SSL_MODE_AUTO_RETRY);

    SSL_set_connect_state(_ssl);
  } else {
    // LOG_DEBUG("SSL has existed.");
  }

  int sslError;
  ret = SSL_connect(_ssl);
  if (ret < 0) {
    sslError = SSL_get_error(_ssl, ret);

    /* err == SSL_ERROR_ZERO_RETURN 
       "SSL_connect: close notify received from peer" */
    // sslError == SSL_ERROR_WANT_X509_LOOKUP
    // SSL_ERROR_SYSCALL
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
      // LOG_DEBUG("sslHandshake continue.");
      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return sslError;
    } else if (sslError == SSL_ERROR_SYSCALL) {
      int errno_code = utility::getLastErrorCode();
      LOG_INFO("SSL(%p) SSL connect error_syscall failed, errno:%d.", this, errno_code);

      if (NLS_ERR_CONNECT_RETRIABLE(errno_code) ||
          NLS_ERR_RW_RETRIABLE(errno_code)) {
        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return SSL_ERROR_WANT_READ;
      } else if (errno_code == 0) {
        LOG_DEBUG("SSL(%p) SSL connect syscall success.", this);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return Success;
      } else {
        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return -(SslConnectFailed);
      }
    } else {
      memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
      const char *SSL_connect_ret = "return of SSL_connect: ";
      memcpy(_errorMsg, SSL_connect_ret, strnlen(SSL_connect_ret, 64));
      ERR_error_string_n(ERR_get_error(),
                         _errorMsg + strnlen(SSL_connect_ret, 64),
                         MAX_SSL_ERROR_LENGTH);
      LOG_ERROR("SSL(%p) SSL connect failed:%s.", this, _errorMsg);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif

      this->sslClose();
      return -(SslConnectFailed);
    }
  } else {
    // LOG_DEBUG("sslHandshake success.");
    #if defined(_MSC_VER)
    ReleaseMutex(_mtxSSL);
    #else
    pthread_mutex_unlock(&_mtxSSL);
    #endif
    return Success;
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxSSL);
#else
  pthread_mutex_unlock(&_mtxSSL);
#endif
  return Success;
}

int SSLconnect::sslWrite(const uint8_t * buffer, size_t len) {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxSSL, INFINITE);
#else
  pthread_mutex_lock(&_mtxSSL);
#endif

  if (_ssl == NULL) {
    LOG_ERROR("SSL(%p) ssl has been closed.", this);

    #if defined(_MSC_VER)
    ReleaseMutex(_mtxSSL);
    #else
    pthread_mutex_unlock(&_mtxSSL);
    #endif
    return -(SslWriteFailed);
  }

  int wLen = SSL_write(_ssl, (void *)buffer, (int)len);
  if (wLen < 0) {
    int eCode = SSL_get_error(_ssl, wLen);
    int errno_code = utility::getLastErrorCode();
    char sslErrMsg[MAX_SSL_ERROR_LENGTH] = {0};
    if (eCode == SSL_ERROR_WANT_READ || eCode == SSL_ERROR_WANT_WRITE) {
      LOG_DEBUG("SSL(%p) Write could not complete. Will be invoked later.", this);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return 0;
    } else if (eCode == SSL_ERROR_SYSCALL) {
      LOG_INFO("SSL(%p) SSL_write error_syscall failed, errno:%d.", this, errno_code);

      if (NLS_ERR_CONNECT_RETRIABLE(errno_code) ||
          NLS_ERR_RW_RETRIABLE(errno_code)) {
        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return 0;
      } else if (errno_code == 0) {
        LOG_DEBUG("SSL(%p) SSL_write syscall success.", this);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return 0;
      } else {
        memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
        const char *SSL_write_ret = "return of SSL_write: ";
        memcpy(sslErrMsg, SSL_write_ret, strnlen(SSL_write_ret, 64));
        ERR_error_string_n(ERR_get_error(),
                           sslErrMsg + strnlen(SSL_write_ret, 64),
                           MAX_SSL_ERROR_LENGTH);
        snprintf(_errorMsg, MAX_SSL_ERROR_LENGTH, "%s. errno_code:%d eCode:%d.", sslErrMsg, errno_code, eCode);
        LOG_ERROR("SSL(%p) SSL_ERROR_SYSCALL Write failed: %s.", this, _errorMsg);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return -(SslWriteFailed);
      }
    } else {
      memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
      const char *SSL_write_ret = "return of SSL_write: ";
      memcpy(sslErrMsg, SSL_write_ret, strnlen(SSL_write_ret, 64));
      ERR_error_string_n(ERR_get_error(),
                         sslErrMsg + strnlen(SSL_write_ret, 64),
                         MAX_SSL_ERROR_LENGTH);
      if (eCode == SSL_ERROR_ZERO_RETURN && errno_code == 0) {
        snprintf(
            _errorMsg, MAX_SSL_ERROR_LENGTH,
            "%s. errno_code:%d eCode:%d. It's mean this connection was closed or shutdown because of bad network.",
            sslErrMsg, errno_code, eCode);
      } else {
        snprintf(_errorMsg, MAX_SSL_ERROR_LENGTH, "%s. errno_code:%d eCode:%d.", sslErrMsg, errno_code, eCode);
      }
      LOG_ERROR("SSL(%p) SSL_write failed: %s.", this, _errorMsg);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return -(SslWriteFailed);
    }
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxSSL);
#else
  pthread_mutex_unlock(&_mtxSSL);
#endif
  return wLen;
}

int SSLconnect::sslRead(uint8_t * buffer, size_t len) {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxSSL, INFINITE);
#else
  pthread_mutex_lock(&_mtxSSL);
#endif

  if (_ssl == NULL) {
    LOG_ERROR("SSL(%p) ssl has been closed.", this);

    #if defined(_MSC_VER)
    ReleaseMutex(_mtxSSL);
    #else
    pthread_mutex_unlock(&_mtxSSL);
    #endif
    return -(SslReadFailed);
  }

  int rLen = SSL_read(_ssl, (void *)buffer, (int)len);
  if (rLen <= 0) {
    int eCode = SSL_get_error(_ssl, rLen);
    int errno_code = utility::getLastErrorCode();
    char sslErrMsg[MAX_SSL_ERROR_LENGTH] = {0};
    //LOG_WARN("Read maybe failed, get_error:%d", eCode);
    if (eCode == SSL_ERROR_WANT_READ ||
        eCode == SSL_ERROR_WANT_WRITE ||
        eCode == SSL_ERROR_WANT_X509_LOOKUP) {
      LOG_WARN("SSL(%p) Read could not complete. Will be invoked later.", this);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return 0;
    } else if (eCode == SSL_ERROR_SYSCALL) {
      LOG_INFO("SSL(%p) SSL_read error_syscall failed, errno:%d.", this, errno_code);

      if (NLS_ERR_CONNECT_RETRIABLE(errno_code) ||
          NLS_ERR_RW_RETRIABLE(errno_code)) {
        LOG_WARN("SSL(%p) Retry read...", this);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return 0;
      } else if (errno_code == 0) {
        LOG_DEBUG("SSL(%p) SSL_write syscall success.", this);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return 0;
      } else {
        memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
        const char *SSL_read_ret = "return of SSL_read: ";
        memcpy(sslErrMsg, SSL_read_ret, strnlen(SSL_read_ret, 64));
        ERR_error_string_n(ERR_get_error(),
                           sslErrMsg + strnlen(SSL_read_ret, 64),
                           MAX_SSL_ERROR_LENGTH);
        snprintf(_errorMsg, MAX_SSL_ERROR_LENGTH, "%s. errno_code:%d eCode:%d.", sslErrMsg, errno_code, eCode);
        LOG_ERROR("SSL(%p) SSL_ERROR_SYSCALL Read failed:, %s.", this, _errorMsg);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return -(SslReadSysError);
      }
    } else {
      memset(_errorMsg, 0x0, MAX_SSL_ERROR_LENGTH);
      const char *SSL_read_ret = "return of SSL_read: ";
      memcpy(sslErrMsg, SSL_read_ret, strnlen(SSL_read_ret, 64));
      ERR_error_string_n(ERR_get_error(),
                         sslErrMsg + strnlen(SSL_read_ret, 64),
                         MAX_SSL_ERROR_LENGTH);
      if (eCode == SSL_ERROR_ZERO_RETURN && errno_code == 0 && ++_sslTryAgain <= MAX_SSL_TRY_AGAIN) {
        snprintf(
            _errorMsg, MAX_SSL_ERROR_LENGTH,
            "%s. errno_code:%d eCode:%d. It's mean this connection was closed or shutdown because of bad network, Try again ...",
            sslErrMsg, errno_code, eCode);
        LOG_WARN("SSL(%p) SSL_read failed: %s.", this, _errorMsg);

        #if defined(_MSC_VER)
        ReleaseMutex(_mtxSSL);
        #else
        pthread_mutex_unlock(&_mtxSSL);
        #endif
        return 0;
      } else {
        snprintf(_errorMsg, MAX_SSL_ERROR_LENGTH, "%s. errno_code:%d eCode:%d.", sslErrMsg, errno_code, eCode);
      }
      LOG_ERROR("SSL(%p) SSL_read failed: %s.", this, _errorMsg);

      #if defined(_MSC_VER)
      ReleaseMutex(_mtxSSL);
      #else
      pthread_mutex_unlock(&_mtxSSL);
      #endif
      return -(SslReadFailed);
    }
  }

  _sslTryAgain = 0;

#if defined(_MSC_VER)
  ReleaseMutex(_mtxSSL);
#else
  pthread_mutex_unlock(&_mtxSSL);
#endif
  return rLen;
}

/**
 * @brief: 关闭TLS/SSL连接
 * @return:
 */
void SSLconnect::sslClose() {
#if defined(_MSC_VER)
  WaitForSingleObject(_mtxSSL, INFINITE);
#else
  pthread_mutex_lock(&_mtxSSL);
#endif

  if (_ssl) {
    LOG_INFO("SSL(%p) ssl connect close.", this);

    SSL_shutdown(_ssl);
    SSL_free(_ssl);
    _ssl = NULL;
  } else {
    LOG_DEBUG("SSL connect has closed.");
  }

#if defined(_MSC_VER)
  ReleaseMutex(_mtxSSL);
#else
  pthread_mutex_unlock(&_mtxSSL);
#endif
}

const char* SSLconnect::getFailedMsg() {
  return _errorMsg;
}

} //AlibabaNls
