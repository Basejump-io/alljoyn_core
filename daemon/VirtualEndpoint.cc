/**
 * @file
 * A VirtualEndpoint is a representation of an AllJoyn endpoint that exists behind a remote
 * AllJoyn daemon.
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
#include <vector>
#include "VirtualEndpoint.h"
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <alljoyn/Message.h>
#include <Status.h>

#define QCC_MODULE "ALLJOYN_OBJ"

using namespace std;
using namespace qcc;

namespace ajn {

VirtualEndpoint::VirtualEndpoint(const char* uniqueName, RemoteEndpoint& b2bEp)
    : BusEndpoint(BusEndpoint::ENDPOINT_TYPE_VIRTUAL),
    m_uniqueName(uniqueName),
    m_hadRefs(false)
{
    m_b2bEndpoints.insert(pair<SessionId, RemoteEndpoint*>(0, &b2bEp));
}

QStatus VirtualEndpoint::PushMessage(Message& msg)
{
    return PushMessage(msg, msg->GetSessionId());
}

QStatus VirtualEndpoint::PushMessage(Message& msg, SessionId id)
{
    QStatus status = ER_BUS_NO_ROUTE;

    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::iterator it = (id == 0) ? m_b2bEndpoints.begin() : m_b2bEndpoints.lower_bound(id);
    while ((it != m_b2bEndpoints.end()) && (id == it->first)) {
        status = it++->second->PushMessage(msg);
        if (status != ER_BUS_ENDPOINT_CLOSING) {
            break;
        }
    }
    m_b2bEndpointsLock.Unlock();

    return status;
}

RemoteEndpoint* VirtualEndpoint::GetBusToBusEndpoint(SessionId sessionId, int* b2bCount) const
{
    RemoteEndpoint* ret = NULL;
    if (b2bCount) {
        *b2bCount = 0;
    }
    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.lower_bound(sessionId);
    while ((it != m_b2bEndpoints.end()) && (it->first == sessionId)) {
        if (!ret) {
            ret = it->second;
        }
        if (b2bCount) {
            (*b2bCount)++;
        }
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
    return ret;
}

bool VirtualEndpoint::AddBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("VirtualEndpoint::AddBusToBusEndpoint(this=%s, b2b=%s)", GetUniqueName().c_str(), endpoint.GetUniqueName().c_str()));

    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    bool found = false;
    while ((it != m_b2bEndpoints.end()) && (it->first == 0)) {
        if (it->second == &endpoint) {
            found = true;
            break;
        }
        ++it;
    }
    if (!found) {
        m_b2bEndpoints.insert(pair<SessionId, RemoteEndpoint*>(0, &endpoint));
    }
    m_b2bEndpointsLock.Unlock();
    return !found;
}

void VirtualEndpoint::GetSessionIdsForB2B(RemoteEndpoint& endpoint, set<SessionId>& sessionIds)
{
    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        if (it->first && (it->second == &endpoint)) {
            sessionIds.insert(it->first);
        }
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
}

bool VirtualEndpoint::RemoveBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("VirtualEndpoint::RemoveBusToBusEndpoint(this=%s, b2b=%s)", GetUniqueName().c_str(), endpoint.GetUniqueName().c_str()));

    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        if (it->second == &endpoint) {
            /* A non-zero session means that the b2b has one less ref */
            if (it->first != 0) {
                it->second->DecrementRef();
            }
            m_b2bEndpoints.erase(it++);
        } else {
            ++it;
        }
    }

    /*
     * Virtual endpoints are removed when they no longer route for any sessions.
     * The exception to this rule is virtual endpoints for the bus controller of remote daemons.
     * These controller virtual endpoints are not removed until all b2b eps are removed
     * regardless of session id.
     */
    bool isEmpty;
    if (m_hadRefs) {
        isEmpty = (m_b2bEndpoints.lower_bound(1) == m_b2bEndpoints.end());
    } else {
        isEmpty = m_b2bEndpoints.empty();
    }
    it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
    return isEmpty;
}

QStatus VirtualEndpoint::AddSessionRef(SessionId id, RemoteEndpoint& b2bEp)
{
    QCC_DbgTrace(("VirtualEndpoint::AddSessionRef(this=%s, id=%u, b2b=%s)", GetUniqueName().c_str(), id, b2bEp.GetUniqueName().c_str()));

    assert(id != 0);

    m_b2bEndpointsLock.Lock();

    /* Sanity check. Make sure b2bEp is connected to this virtual ep (with sessionId == 0) */
    bool canUse = CanUseRoute(b2bEp);
    if (canUse) {
        /* Increment b2bEp ref */
        b2bEp.IncrementRef();
        /* Map sessionId to b2bEp */
        m_b2bEndpoints.insert(pair<SessionId, RemoteEndpoint*>(id, &b2bEp));
        m_hadRefs = true;
    }
    m_b2bEndpointsLock.Unlock();
    return canUse ? ER_OK : ER_FAIL;
}

QStatus VirtualEndpoint::AddSessionRef(SessionId id, SessionOpts* opts, RemoteEndpoint*& b2bEp)
{
    QCC_DbgTrace(("VirtualEndpoint::AddSessionRef(this=%s, %u, <opts>, %s)", GetUniqueName().c_str(), id, b2bEp ? b2bEp->GetUniqueName().c_str() : "<none>"));

    RemoteEndpoint* bestEp = NULL;
    //uint32_t hops = 1000;

    m_b2bEndpointsLock.Lock();

#if 0
    /* Look for best B2B that matches SessionOpts */
    multimap<SessionId, RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.begin();
    while ((it != m_b2bEndpoints.end()) && (it->first == 0)) {
        map<RemoteEndpoint*, B2BInfo>::const_iterator bit = m_b2bInfos.find(it->second);
        if ((bit != m_b2bInfos.end()) && (!opts || bit->second.opts.IsCompatible(*opts)) && (bit->second.hops < hops)) {
            bestEp = it->second;
            hops = bit->second.hops;
        }
        ++it;
    }
#else
    /* TODO: Placeholder until we exchange session opts and hop count via ExchangeNames */
    multimap<SessionId, RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.find(id);
    if (it == m_b2bEndpoints.end()) {
        it = m_b2bEndpoints.begin();
    }
    if ((it != m_b2bEndpoints.end()) && ((it->first == 0) || it->first == id)) {
        bestEp = it->second;
    }
#endif

    /* Map session id to bestEp */
    if (bestEp) {
        AddSessionRef(id, *bestEp);
    }
    b2bEp = bestEp;
    m_b2bEndpointsLock.Unlock();
    return bestEp ? ER_OK : ER_FAIL;
}

void VirtualEndpoint::RemoveSessionRef(SessionId id)
{
    QCC_DbgTrace(("VirtualEndpoint::RemoveSessionRef(this=%s, id=%u)", GetUniqueName().c_str(), id));
    assert(id != 0);
    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::iterator it = m_b2bEndpoints.find(id);
    if (it != m_b2bEndpoints.end()) {
        it->second->DecrementRef();
        m_b2bEndpoints.erase(it);
    } else {
        QCC_DbgPrintf(("VirtualEndpoint::RemoveSessionRef: vep=%s failed to find session = %u", m_uniqueName.c_str(), id));
    }
    m_b2bEndpointsLock.Unlock();
}

bool VirtualEndpoint::CanUseRoute(const RemoteEndpoint& b2bEndpoint) const
{
    bool isFound = false;
    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.begin();
    while ((it != m_b2bEndpoints.end()) && (it->first == 0)) {
        if (it->second == &b2bEndpoint) {
            isFound = true;
            break;
        }
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
    return isFound;
}

bool VirtualEndpoint::CanRouteWithout(const qcc::GUID& guid) const
{
    bool canRoute = false;
    m_b2bEndpointsLock.Lock();
    multimap<SessionId, RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        if (guid != it->second->GetRemoteGUID()) {
            canRoute = true;
            break;
        }
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
    return canRoute;
}

}
