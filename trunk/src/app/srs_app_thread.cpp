/*
The MIT License (MIT)

Copyright (c) 2013-2015 winlin

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <srs_app_thread.hpp>

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

ISrsThreadHandler::ISrsThreadHandler()
{
}

ISrsThreadHandler::~ISrsThreadHandler()
{
}

void ISrsThreadHandler::on_thread_start()
{
}

int ISrsThreadHandler::on_before_cycle()
{
    int ret = ERROR_SUCCESS;
    return ret;
}

int ISrsThreadHandler::on_end_cycle()
{
    int ret = ERROR_SUCCESS;
    return ret;
}

void ISrsThreadHandler::on_thread_stop()
{
}

SrsThread::SrsThread(const char* name, ISrsThreadHandler* thread_handler, int64_t interval_us, bool joinable)
{
    _name = name;
    handler = thread_handler;
    cycle_interval_us = interval_us;
    
    tid = NULL;
    loop = false;
    really_terminated = true;
    _cid = -1;
    _joinable = joinable;
    
    // in start(), the thread cycle method maybe stop and remove the thread itself,
    // and the thread start() is waiting for the _cid, and segment fault then.
    // @see https://github.com/winlinvip/simple-rtmp-server/issues/110
    // thread will set _cid, callback on_thread_start(), then wait for the can_run signal.
    can_run = false;
}

SrsThread::~SrsThread()
{
    stop();
}

int SrsThread::cid()
{
    return _cid;
}

int SrsThread::start()
{
    int ret = ERROR_SUCCESS;
    
    if(tid) {
        srs_info("thread %s already running.", _name);
        return ret;
    }
    
    if((tid = st_thread_create(thread_fun, this, (_joinable? 1:0), 0)) == NULL){
        ret = ERROR_ST_CREATE_CYCLE_THREAD;
        srs_error("st_thread_create failed. ret=%d", ret);
        return ret;
    }
    
    // we set to loop to true for thread to run.
    loop = true;
    
    // wait for cid to ready, for parent thread to get the cid.
    while (_cid < 0 && loop) {
        st_usleep(10 * 1000);
    }
    
    // now, cycle thread can run.
    can_run = true;
    
    return ret;
}

void SrsThread::stop()
{
    if (tid) {
        loop = false;
        
        // the interrupt will cause the socket to read/write error,
        // which will terminate the cycle thread.
        st_thread_interrupt(tid);
        
        // when joinable, wait util quit.
        if (_joinable) {
            // wait the thread to exit.
            int ret = st_thread_join(tid, NULL);
            if (ret) {
                srs_warn("core: ignore join thread failed.");
            }

            // wait the thread actually terminated.
            // sometimes the thread join return -1, for example, 
            // when thread use st_recvfrom, the thread join return -1.
            // so here, we use a variable to ensure the thread stopped.
            while (!really_terminated) {
                st_usleep(10 * 1000);

                if (really_terminated) {
                    break;
                }
                srs_warn("core: wait thread to actually terminated");
            }
        }
        
        tid = NULL;
    }
}

bool SrsThread::can_loop()
{
    return loop;
}

void SrsThread::stop_loop()
{
    loop = false;
}

void SrsThread::thread_cycle()
{
    int ret = ERROR_SUCCESS;
    
    _srs_context->generate_id();
    srs_info("thread %s cycle start", _name);
    
    _cid = _srs_context->get_id();
    
    srs_assert(handler);
    handler->on_thread_start();

    // thread is running now.
    really_terminated = false;
    
    // wait for cid to ready, for parent thread to get the cid.
    while (!can_run && loop) {
        st_usleep(10 * 1000);
    }
    
    while (loop) {
        if ((ret = handler->on_before_cycle()) != ERROR_SUCCESS) {
            srs_warn("thread %s on before cycle failed, ignored and retry, ret=%d", _name, ret);
            goto failed;
        }
        srs_info("thread %s on before cycle success");
        
        if ((ret = handler->cycle()) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_warn("thread %s cycle failed, ignored and retry, ret=%d", _name, ret);
            }
            goto failed;
        }
        srs_info("thread %s cycle success", _name);
        
        if ((ret = handler->on_end_cycle()) != ERROR_SUCCESS) {
            srs_warn("thread %s on end cycle failed, ignored and retry, ret=%d", _name, ret);
            goto failed;
        }
        srs_info("thread %s on end cycle success", _name);

failed:
        if (!loop) {
            break;
        }
        
        // to improve performance, donot sleep when interval is zero.
        // @see: https://github.com/winlinvip/simple-rtmp-server/issues/237
        if (cycle_interval_us != 0) {
            st_usleep(cycle_interval_us);
        }
    }
    
    handler->on_thread_stop();
    srs_info("thread %s cycle finished", _name);

    // readly terminated now.
    really_terminated = true;
}

void* SrsThread::thread_fun(void* arg)
{
    SrsThread* obj = (SrsThread*)arg;
    srs_assert(obj);
    
    obj->thread_cycle();

    st_thread_exit(NULL);
    
    return NULL;
}

