/**
 * @file
 */

/******************************************************************************
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/
#include <qcc/platform.h>

#include <assert.h>

#include <qcc/Debug.h>
#include <qcc/String.h>
#include <qcc/atomic.h>
#include <qcc/Thread.h>

#include <alljoyn/BusAttachment.h>

#include "Router.h"
#include "RemoteEndpoint.h"
#include "LocalTransport.h"
#include "AllJoynPeerObj.h"
#include "BusInternal.h"

#ifndef NDEBUG
#include <qcc/time.h>
#endif

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

#define ENDPOINT_IS_DEAD_ALERTCODE  1


/* Endpoint constructor */
RemoteEndpoint::RemoteEndpoint(BusAttachment& bus,
                               bool incoming,
                               const qcc::String& connectSpec,
                               Stream& stream,
                               const char* threadName) :
    BusEndpoint(BusEndpoint::ENDPOINT_TYPE_REMOTE),
    bus(bus),
    auth(bus, stream, incoming),
    txQueue(),
    txWaitQueue(),
    txQueueLock(),
    exitCount(0),
    rxThread(bus, (qcc::String(incoming ? "rx-srv-" : "rx-cli-") + threadName).c_str(), stream, incoming),
    txThread(bus, (qcc::String(incoming ? "tx-srv-" : "tx-cli-") + threadName).c_str(), stream, txQueue, txWaitQueue, txQueueLock),
    connSpec(connectSpec),
    incoming(incoming),
    allowRemote(false)
{
}

RemoteEndpoint::~RemoteEndpoint()
{
    /* Request Stop */
    Stop();

    /* Wait for thread to shutdown */
    Join();
}

QStatus RemoteEndpoint::Start(bool isBusToBus, bool allowRemote)
{
    QCC_DbgTrace(("RemoteEndpoint::Start(isBusToBus = %s, allowRemote = %s)",
                  isBusToBus ? "true" : "false",
                  allowRemote ? "true" : "false"));
    QStatus status;
    Router& router = bus.GetInternal().GetRouter();
    bool isTxStarted = false;
    bool isRxStarted = false;

    if (isBusToBus) {
        endpointType = BusEndpoint::ENDPOINT_TYPE_BUS2BUS;
    }
    this->allowRemote = allowRemote;

    /* Start the TX thread */
    status = txThread.Start(this, this);
    isTxStarted = (ER_OK == status);

    /* Register endpoint */
    if (ER_OK == status) {
        status = router.RegisterEndpoint(*this, false);
    }

    /* Start the Rx thread */
    if (ER_OK == status) {
        status = rxThread.Start(this, this);
        isRxStarted = (ER_OK == status);
    }

    /* If thread failed to start, then unregister. */
    if (ER_OK != status) {
        if (isTxStarted) {
            txThread.Stop();
            txThread.Join();
        }
        if (isRxStarted) {
            rxThread.Stop();
            rxThread.Join();
        }
        router.UnregisterEndpoint(*this);
        QCC_LogError(status, ("AllJoynRemoteEndoint::Start failed"));
    }

    return status;
}

void RemoteEndpoint::SetListener(EndpointListener* listener)
{
    this->listener = listener;
}

QStatus RemoteEndpoint::Stop(void)
{
    QStatus rxStatus, txStatus;

    /* Alert any threads that are on the wait queue */
    txQueueLock.Lock();
    deque<Thread*>::iterator it = txWaitQueue.begin();
    while (it != txWaitQueue.end()) {
        (*it++)->Alert(ENDPOINT_IS_DEAD_ALERTCODE);
    }
    txQueueLock.Unlock();


    rxStatus = rxThread.Stop();
    txStatus = txThread.Stop();
    return (ER_OK == rxStatus) ? txStatus : rxStatus;
}

QStatus RemoteEndpoint::Join(void)
{
    /* Join any threads that are on the wait queue */
    txQueueLock.Lock();
    deque<Thread*>::iterator it = txWaitQueue.begin();
    while (it != txWaitQueue.end()) {
        (*it++)->Join();
    }
    txQueueLock.Unlock();

    return ER_OK;
}

void RemoteEndpoint::ThreadExit(Thread* thread)
{
    /* If one thread stops, the other must too */
    if ((&rxThread == thread) && txThread.IsRunning()) {
        txThread.Stop();
    } else if (rxThread.IsRunning()) {
        rxThread.Stop();
    }

    /* Unregister endpoint when both rx and tx exit */
    if (2 == IncrementAndFetch(&exitCount)) {
        /* De-register this remote endpoint */
        bus.GetInternal().GetRouter().UnregisterEndpoint(*this);
        if (NULL != listener) {
            listener->EndpointExit(this);
        }
    }
}

void* RemoteEndpoint::RxThread::Run(void* arg)
{
    QStatus status = ER_OK;
    RemoteEndpoint* ep = reinterpret_cast<RemoteEndpoint*>(arg);

    /* Receive messages until the socket is disconnected */
    Router& router = bus.GetInternal().GetRouter();
    while (!IsStopping() && (ER_OK == status)) {
        status = Event::Wait(source.GetSourceEvent());
        if (ER_OK == status) {
            Message msg(bus);
            status = msg->Unmarshal(source, ep->GetUniqueName(), (validateSender && BusEndpoint::ENDPOINT_TYPE_BUS2BUS != ep->GetEndpointType()));
            switch (status) {
            case ER_OK :
                status = router.PushMessage(msg, *ep);
                if ((status == ER_BUS_SIGNATURE_MISMATCH) || (status == ER_BUS_UNMATCHED_REPLY_SERIAL)) {
                    QCC_LogError(status, ("Discarding %s", msg->Description().c_str()));
                    status = ER_OK;
                }
                break;

            case ER_BUS_CANNOT_EXPAND_MESSAGE :
                /*
                 * The message could not be expanded so pass it the peer object to request the expansion
                 * rule from the endpoint that sent it.
                 */
                status = bus.GetInternal().GetLocalEndpoint().GetPeerObj()->RequestHeaderExpansion(msg, ep);
                break;

            case ER_BUS_TIME_TO_LIVE_EXPIRED:
                QCC_DbgHLPrintf(("TTL expired discarding %s", msg->Description().c_str()));
                status = ER_OK;
                break;

            case ER_BUS_INVALID_HEADER_SERIAL:
                /*
                 * Ignore invalid serial numbers for unreliable messages or for broadcast messages that come from
                 * bus2bus endpoints because these can be delivered out-of-order or multiple times.
                 * In all other cases an invalid serial number cause the connection to be dropped.
                 *
                 * Also, Hello/BusHello messages are considered out of sequence since they specify org.freedesktop.DBus
                 * as the sender which is a name whose PeerState is already in use at the time of the message.
                 */
                if (msg->IsUnreliable() ||
                    ((ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_BUS2BUS) && (*(msg->GetDestination()) == '\0')) ||
                    (strcmp("org.freedesktop.DBus", msg->GetInterface()) == 0) ||
                    (strcmp("org.alljoyn.Bus", msg->GetInterface()) == 0)) {
                    QCC_DbgHLPrintf(("Invalid serial (%u) number from %s (%s,%s). discarding", msg->GetCallSerial(),
                                     ep->GetUniqueName().c_str(), msg->GetInterface(), msg->GetMemberName()));
                    status = ER_OK;
                } else {
                    QCC_LogError(ER_FAIL, ("ER_BUS_INVALID_HEADER_SERIAL (%u) with iface=%s, member=%s",
                                           msg->GetCallSerial(), msg->GetInterface(), msg->GetMemberName()));
                }
                break;

            default:
                break;
            }
        }
    }

    if ((ER_OK != status) && (ER_STOPPING_THREAD != status) && (ER_SOCK_OTHER_END_CLOSED != status)) {
        QCC_LogError(status, ("Endpoint Rx thread (%s) exiting", GetName().c_str()));
    }

    /* On an unexpected disconnect save the status that cause the thread exit */
    if (ep->disconnectStatus == ER_OK) {
        ep->disconnectStatus = (status == ER_STOPPING_THREAD) ? ER_OK : status;
    }

    /* Inform transport of endpoint exit */
    return (void*) status;
}

void* RemoteEndpoint::TxThread::Run(void* arg)
{
    QStatus status = ER_OK;
    RemoteEndpoint* ep = reinterpret_cast<RemoteEndpoint*>(arg);

    /* Wait for queue to be non-empty */
    while (!IsStopping() && (ER_OK == status)) {

        status = Event::Wait(Event::neverSet);

        if (!IsStopping() && (ER_ALERTED_THREAD == status)) {
            stopEvent.ResetEvent();
            status = ER_OK;
            while (!queue.empty() && !IsStopping()) {
                queueLock.Lock();

                /* Get next message */
                Message msg = queue.back();
                queue.pop_back();

                /* Alert next thread on wait queue */
                if (0 < waitQueue.size()) {
                    Thread* wakeMe = waitQueue.back();
                    waitQueue.pop_back();
                    status = wakeMe->Alert();
                    if (ER_OK != status) {
                        QCC_LogError(status, ("Failed to alert thread blocked on full tx queue"));
                    }
                }

                queueLock.Unlock();

                /* Deliver message */
                status = msg->Deliver(sink);
            }
        }
    }

    /* Wake any thread waiting on tx queue availability */
    queueLock.Lock();
    while (0 < waitQueue.size()) {
        Thread* wakeMe = waitQueue.back();
        QStatus status = wakeMe->Alert();
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to clear tx wait queue"));
        }
        waitQueue.pop_back();
    }
    queueLock.Unlock();

    /* On an unexpected disconnect save the status that cause the thread exit */
    if (ep->disconnectStatus == ER_OK) {
        ep->disconnectStatus = (status == ER_STOPPING_THREAD) ? ER_OK : status;
    }

    /* Inform transport of endpoint exit */
    return (void*) status;
}

QStatus RemoteEndpoint::PushMessage(Message& msg)
{
    static const size_t MAX_TX_QUEUE_SIZE = 10;

    QStatus status = ER_OK;

    /*
     * Don't continue if this endpoint is in the process of being closed
     * Otherwise we risk deadlock when sending NameOwnerChanged signal to
     * this dying endpoint
     */
    if (rxThread.IsStopping() || txThread.IsStopping()) {
        return ER_BUS_ENDPOINT_CLOSING;
    }
    txQueueLock.Lock();
    size_t count = txQueue.size();
    bool wasEmpty = (count == 0);
    if (MAX_TX_QUEUE_SIZE > count) {
        txQueue.push_front(msg);
    } else {
        while (true) {
            /* Remove a queue entry whose TTLs is expired if possible */
            deque<Message>::iterator it = txQueue.begin();
            uint32_t maxWait = 20 * 1000;
            while (it != txQueue.end()) {
                uint32_t expMs;
                if ((*it)->IsExpired(&expMs)) {
                    txQueue.erase(it);
                    break;
                } else {
                    ++it;
                }
                maxWait = (std::min)(maxWait, expMs);
            }
            if (txQueue.size() < MAX_TX_QUEUE_SIZE) {
                /* Check queue wasn't drained while we were waiting */
                if (txQueue.size() == 0) {
                    wasEmpty = true;
                }
                txQueue.push_front(msg);
                status = ER_OK;
                break;
            } else {
                Thread* thread = Thread::GetThread();
                assert(thread);

                /* This thread will have to wait for room in the queue */
                txWaitQueue.push_front(thread);
                txQueueLock.Unlock();
                status = Event::Wait(Event::neverSet, maxWait);
                if (ER_ALERTED_THREAD == status) {
                    if (thread->GetAlertCode() == ENDPOINT_IS_DEAD_ALERTCODE) {
                        /* The endpoint is gone so don't touch the object */
                        return ER_BUS_ENDPOINT_CLOSING;
                    } else {
                        thread->GetStopEvent().ResetEvent();
                    }
                } else {
                    /* There was a timeout or some other non-expected exit from wait. Remove thread from wait queue. */
                    /* If thread isn't on queue, this means there is an alert in progress that we must clear */
                    txQueueLock.Lock();
                    bool foundThread = false;
                    deque<Thread*>::iterator eit = txWaitQueue.begin();
                    while (eit != txWaitQueue.end()) {
                        if (*eit == thread) {
                            txWaitQueue.erase(eit);
                            foundThread = true;
                            break;
                        }
                        ++eit;
                    }
                    txQueueLock.Unlock();
                    if (!foundThread) {
                        thread->GetStopEvent().ResetEvent();
                    }
                }
                txQueueLock.Lock();
                if ((ER_OK != status) && (ER_ALERTED_THREAD != status) && (ER_TIMEOUT != status)) {
                    break;
                }
            }
        }
    }
    txQueueLock.Unlock();

    if (wasEmpty) {
        status = txThread.Alert();
    }

#ifndef NDEBUG
#undef QCC_MODULE
#define QCC_MODULE "TXSTATS"
    static uint32_t lastTime = 0;
    uint32_t now = GetTimestamp();
    if ((now - lastTime) > 1000) {
        QCC_DbgPrintf(("Tx queue size (%s - %x) = %d", txThread.GetName().c_str(), txThread.GetHandle(), count));
        lastTime = now;
    }
#undef QCC_MODULE
#define QCC_MODULE "ALLJOYN"
#endif

    return status;
}

}
