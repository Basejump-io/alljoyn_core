/**
 * @file
 * The AllJoyn P2P Connection Manager Implementation
 */

/******************************************************************************
 * Copyright 2012, Qualcomm Innovation Center, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include <vector>

#include <qcc/Debug.h>
#include <qcc/Event.h>
#include <qcc/time.h>

#include <ns/IpNameService.h>

#include "P2PConMan.h"
#include "P2PConManImpl.h"

#define QCC_MODULE "P2PCM"

namespace ajn {

P2PConManImpl::P2PConManImpl()
    : m_state(IMPL_SHUTDOWN), m_myP2pHelperListener(0), m_p2pHelperInterface(0), m_bus(0),
    m_connState(CONN_IDLE), m_l2thread(0), m_l3thread(0), m_callback(0)
{
    QCC_DbgPrintf(("P2PConManImpl::P2PConManImpl()"));
}

P2PConManImpl::~P2PConManImpl()
{
    QCC_DbgPrintf(("P2PConManImpl::~P2PConManImpl()"));

    //
    // Delete our instace of the P2P Helper we used to communicate with the P2P
    // Helper Service, and the listener object that plumbed events back from the
    // helper to us.
    //
    delete m_p2pHelperInterface;
    m_p2pHelperInterface = NULL;
    delete m_myP2pHelperListener;
    m_myP2pHelperListener = NULL;

    //
    // All shut down and ready for bed.
    //
    m_state = IMPL_SHUTDOWN;
}

QStatus P2PConManImpl::Init(BusAttachment* bus, const qcc::String& guid)
{
    QCC_DbgPrintf(("P2PConManImpl::Init()"));

    //
    // Can only call Init() if the object is not running or in the process
    // of initializing
    //
    if (m_state != IMPL_SHUTDOWN) {
        return ER_FAIL;
    }

    m_state = IMPL_INITIALIZING;
    m_bus = bus;
    m_guid = guid;

    if (m_p2pHelperInterface == NULL) {
        QCC_DbgPrintf(("P2PConManImpl::Init(): new P2PHelperInterface"));
        m_p2pHelperInterface = new P2PHelperInterface();
        m_p2pHelperInterface->Init(bus);

        assert(m_myP2pHelperListener == NULL && "P2PConManImpl::Init(): m_pyP2pHelperListener must be NULL");
        QCC_DbgPrintf(("P2PConManImpl::Init(): new P2PHelperListener"));
        m_myP2pHelperListener = new MyP2PHelperListener(this);
        m_p2pHelperInterface->SetListener(m_myP2pHelperListener);
    }
    return ER_OK;
}

QStatus P2PConManImpl::SetCallback(Callback<void, P2PConMan::LinkState, const qcc::String&>* cb)
{
    QCC_DbgHLPrintf(("P2PConManImpl::SetCallback()"));

    if (m_state != IMPL_RUNNING) {
        QCC_DbgPrintf(("P2PConManImpl::SetCallback(): Not IMPL_RUNNING"));
        return ER_FAIL;
    }

    Callback<void, P2PConMan::LinkState, const qcc::String&>* goner = m_callback;
    m_callback = NULL;
    delete goner;
    m_callback = cb;

    return ER_OK;
}

QStatus P2PConManImpl::CreateTemporaryNetwork(const qcc::String& device, int32_t goIntent)
{
    //
    // This method does one of two things depending on the provided goIntent.
    // The goIntent corresponds to a Group Owner Intent Attribute as used in the
    // Wi-Fi P2P GO Negotiation request with the following changes in
    // interpretation: Where the P2P spec indicates a relative value indicating
    // the desire of the P2P device to be a Group Owner, we define the value
    // zero as indicating an absolute requirement that the device must be a STA
    // (as seen in the constant P2PConMan::DEVICE_MUST_BE_STA = 0) and the value
    // 15 indicates an absolute requirement that the device must be a GO (as
    // seen in the constant P2PConMan::DEVICE_MUST_BE_GO = 15).
    //
    // When we actually make the RPC call to establish the link, which we do by
    // calling into our helper object's EstablishLinkAsync method, if we are
    // a client, we expect a goIntent indicating that the device must be the STA
    // and we also expect the temporary network to be formed at that time.  If
    // we are a service, we expect the goIntent to be 15, but since there is no
    // client involved yet, there cannot be an actual temporary network formed.
    // Such a call, with goIntent set to 15 is treated as an advisory message
    // by the framework.  We are telling it that we are the service here, and
    // we should be expecting remote AllJoyn clients to try and connect as the
    // STA nodes in the relationship.
    //
    // A sucessful response to EstablishLinkAsync in the case of a service (GO)
    // indicates that the framework is capable of accepting remote connections
    // from STA nodes, not that a network has actually been formed.  When we
    // return success from CreateTemporaryNetwork in the case of a GO, we are
    // also communicating the fact that, as far as we can tell, we are ready
    // to accept incoming connections.
    //
    QCC_DbgHLPrintf(("P2PConManImpl::CreateTemporaryNetwork(): device = \"%s\", intent = %d.", device.c_str(), goIntent));

    if (m_state != IMPL_RUNNING) {
        QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): Not IMPL_RUNNING"));
        return ER_FAIL;
    }

    //
    //
    // We only allow one thread at a time to be in here trying to make or
    // destroy a connection.  This means that the last thread to try and
    // establish or release a connnection will win.  We expect that the shim
    // that talks to the Android Application Framework will be smart enough to
    // tear down any existing link if we "establish" over it so we don't bother
    // to do the release which would mean blocking and trying to figure out that
    // an OnLinkLost actually came back for the link we are releasing.
    //
    m_establishLock.Lock(MUTEX_CONTEXT);

    //
    // Since we are now supposed to be the only thread dealing with layer two
    // connections, we expect that a previous thread has cleaned up after itself.
    //
    assert(m_l2thread == NULL && "P2PConManImpl::CreateTemporaryNetwork(): m_l2thread was left set");

    //
    // XXX BUGBUG FIXME
    //
    // Apparently there is no way in the Android Application Framework to allow
    // the device receiving a connection request to convey a Group Owner Intent.
    // This means that in the case of a client/server relationship, the server
    // cannot specify that it needs to be the GO.
    //
    // We wanted to sue an establish link with no remote device and a GO intent
    // of fifteen as an advisory message to communicate this fact.  It can't
    // work as it stands, so we don't bother telling the P2P Helper Service
    // since down't know what to do with it.  So if we see DEVICE_MUST_BE_GO we
    // just ignore the request.  We will get a callback from the Framework, via
    // the P2P Helper Service when the link is finally established, at which
    // pont we just remember the handle that is returned there and go to
    // CONN_CONNECTED directly in the callback.
    //
    if (goIntent == P2PConMan::DEVICE_MUST_BE_GO) {
        QCC_LogError(ER_FAIL, ("P2PConManImpl::CreateTemporaryNetwork(): FIXME: GO intent request ignored temporarily."));
        m_connState = CONN_READY;
        m_establishLock.Unlock(MUTEX_CONTEXT);
        return ER_OK;
    }

    //
    // Ask the Wi-Fi P2P helper to join the P2P group owned by the device we
    // found.  In order to make Wi-Fi P2P work remotely reasonably, we the
    // device that advertises a service must act as the group owner (GO) and the
    // device which uses the service must act as a station (STA) node.  It's not
    // really our business, so we allow any arbitrary intent, but that should
    // be the expectation.
    //
    // What we do believe and enforce is that we support only one connection
    // (either GO or STA) and if a new request comes in, we just summarily
    // blow away any existing connection irrespective of what state it may be
    // in.
    //
    m_connState = CONN_CONNECTING;
    m_device = device;

    //
    // We are about to make an asynchrounous call out to the P2P Helper Service
    // which will, in turn, call into the Android Application Framework to make
    // a Wi-Fi Direct request.  There are several outcomes to the method call
    // we are about to make (EstablishLinkAsync):
    //
    // 1) The HandleEstablishLinkReply returns an error, in which case the
    //    CreateTemporaryNetwork process has failed.
    //
    // 2) The HandleEstablishLinkReply returns "no error," in which case we need
    //    to look at the outcome of the asynchronous operation which comes back
    //    in either the OnLinkEstablished() or OnLinkError() callbacks.
    //
    // 3) If OnLinkEstablished() returns us a handle, then we have established
    //    a temporary netowrk.  If OnLinkError returns instead, then we were
    //    unable to create the network for what may be a permanent or temporary
    //    error.  We don't know which, but all we know is that the temporary
    //    network creation failed.
    //
    // 4) If neither OnLinkEstablished() or OnLinkError() returns, then the
    //    P2P Helper service has most likely gone away.  In this case, there is
    //    nothing we can do but fail and hope it comes back later.
    //
    // 5) Even if all goes well, we may get an OnLinkList() callback at any time
    //    that indicates that our temporary network has disassociated and we've
    //    lost it.  If this happens, we rely on the transports (in the networking
    //    sense) to indicate to the WFD transport that connections have been lost.
    //
    // We have a number of flags that will indicate that the various callbacks
    // happened.  As soon as we enter the method call below, they can start
    // happening since we are protecting ourselves from other high-level callers
    // with the mutex above.  So we need to reset all of these flags before we
    // make the call.  Whenever one of the callbacks happens, it needs to alert
    // our thread so we can wake up and process; so we save our thread ID.
    //
    m_l2thread = qcc::Thread::GetThread();
    m_handleEstablishLinkReplyFired = false;
    m_onLinkErrorFired = false;
    m_onLinkEstablishedFired = false;

    QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync()"));
    QStatus status = m_p2pHelperInterface->EstablishLinkAsync(device.c_str(), goIntent);
    if (status != ER_OK) {
        m_threadLock.Lock();
        if (m_l2thread->GetAlertCode() == PRIVATE_ALERT_CODE) {
            m_l2thread->GetStopEvent().ResetEvent();
            m_l2thread->ResetAlertCode();
        }
        m_l2thread = NULL;
        m_handle = -1;
        m_device = "";
        m_interface = "";
        m_connState = CONN_IDLE;
        m_threadLock.Unlock();

        m_establishLock.Unlock(MUTEX_CONTEXT);
        QCC_LogError(status, ("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync(): Call failure"));
        return status;
    }

    qcc::Timespec tTimeout = TEMPORARY_NETWORK_ESTABLISH_TIMEOUT;
    qcc::Timespec tStart;
    qcc::GetTimeNow(&tStart);

    for (;;) {
        //
        // We always expect a reply to our asynchronous call.  We ignore it if
        // the reply indicates no error happened, but we need to fail/bail if
        // there was an error.
        //
        // If the call succeeded, it returned a handle that we can use to
        // associate further responses with the current instance of the
        // establish link call.  This handle is stored in m_handle, not
        // too surprisingly.  The presence of one handle variable reflects
        // the choice of one and only one P2P connection at a time.
        //
        // If we have asked to be the GO in the temporary network, we have
        // really just advised the P2P Helper Service of our requirement to be
        // the GO.  We don't/can't wait around until a link is actually
        // established, so we go head and break out if the service tells us that
        // it agrees.
        //
        if (m_handleEstablishLinkReplyFired) {
            if (m_establishLinkResult != ER_OK) {
                status = ER_P2P;
                QCC_LogError(status, ("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync(): Reply failure"));
                break;
            } else {
                QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync(): Reply success"));
                if (goIntent == P2PConMan::DEVICE_MUST_BE_GO) {
                    QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): GO intent acknowledged"));
                    break;
                }
            }
        }

        //
        // If the onLinkError callback fires, it means that the P2P Helper Service
        // tried to call down into the Android Application Framework, but couldn't
        // arrange for the network to be started.  There's nothing we can do but
        // report the problem and give up.
        //
        if (m_onLinkErrorFired) {
            status = ER_P2P;
            QCC_LogError(status, ("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync(): OnLinkError(%d)", m_linkError));
            break;
        }

        //
        // If the OnLinkEstablished() callback fires, then we have succeeded in
        // arranging for a temporary network to be started, and if we are a STA
        // in the resulting network, the device on the other side has
        // authenticated and we are ready to go.
        //
        // We set our state to CONN_CONNECTED, we expect that m_handle was set by
        // OnLinkEstablished(), and m_device was set above.  These three tidbits
        // identify that we are up and connected with a remote device of some
        // flavor.  What is missing is the name of the network interface that
        // the Wi-Fi Direct part of the Android Application Framework has used.
        // It is probably "p2p0" but we make sure of that later.
        //
        if (m_onLinkEstablishedFired) {
            m_connState = CONN_CONNECTED;
            status = ER_OK;
            break;
        }

        //
        // Wait for something interesting to happen, but not too long.  Only
        // wait until a cummulative time from the starting time before declaring
        // a timeout.
        //
        qcc::Timespec tNow;
        qcc::GetTimeNow(&tNow);

        QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): tStart == %d", tStart.GetAbsoluteMillis()));
        QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): tTimeout == %d", tTimeout.GetAbsoluteMillis()));
        QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): tNow == %d", tNow.GetAbsoluteMillis()));

        if (tNow < tStart + tTimeout) {
            qcc::Timespec tWait = tStart + tTimeout - tNow;
            QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): tWait == %d", tWait.GetAbsoluteMillis()));
            qcc::Event evt(tWait.GetAbsoluteMillis(), 0);
            QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): Wait for something to happen"));
            status = qcc::Event::Wait(evt);
            QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): Something happened"));

            //
            // We use Alert(PRIVATE_ALERT_CODE) in our callbacks to unblock the
            // thread from the Event::Wait() above if an interesting event
            // happens.  This causes the wait to return with ER_ALERTED_THREAD.
            // If we see this error, we look to see if the thread was alerted by
            // us.  This is the case if we see our private alert code.
            //
            // If it was not us precipitating the Alert(), we return an error
            // since someone else needs us to stop what we are doing.  In
            // particular, the system might be going down so we can't just hang
            // around here and arbitrarily keep that from happening.
            //
            // If it was us who caused the Alert(), we reset the stop event that
            // Alert() is using under the sheets and pop up to take a look around
            // and see what happened.
            //
            if (status == ER_ALERTED_THREAD) {
                QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): Something happened: Alerted thread"));
                assert(m_l2thread != NULL && "P2PConManImpl::CreateTemporaryNetwork(): m_l2thread must not be NULL");
                if (m_l2thread->GetAlertCode() == PRIVATE_ALERT_CODE) {
                    m_l2thread->GetStopEvent().ResetEvent();
                    m_l2thread->ResetAlertCode();
                    status = ER_OK;
                } else {
                    QCC_LogError(status, ("P2PConManImpl::CreateTemporaryNetwork(): Thread has been Alert()ed"));
                    break;
                }
            }
        } else {
            status = ER_P2P_TIMEOUT;
            QCC_LogError(status, ("P2PConManImpl::CreateTemporaryNetwork(): EstablishLinkAsync(): Timeout"));
            break;
        }
    }

    QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): Out of loop.  Status = %s", QCC_StatusText(status)));

    //
    // If we didn't succeed, we go back into the idle state and stand ready for
    // another connection attempt.
    //
    if (status != ER_OK) {
        m_handle = -1;
        m_device = "";
        m_interface = "";
        m_connState = CONN_IDLE;
    }

    //
    // The thread that called to start this whole deal is returning to what
    // fate we do not know.  We don't want to affect it any more, so we
    // need to forget about it.  But before we send it on its way, we need
    // to make sure that we don't leave the thread alerted.  Bad things can
    // happen if the calling thread doesn't expect this.
    //
    m_threadLock.Lock();
    if (m_l2thread->GetAlertCode() == PRIVATE_ALERT_CODE) {
        m_l2thread->GetStopEvent().ResetEvent();
        m_l2thread->ResetAlertCode();
    }
    m_l2thread = NULL;
    m_threadLock.Unlock();

    m_establishLock.Unlock(MUTEX_CONTEXT);
    return status;
}

QStatus P2PConManImpl::DestroyTemporaryNetwork(const qcc::String& device, uint32_t intent)
{
    QCC_DbgHLPrintf(("P2PConManImpl::DestroyTemporaryNetwork(): \"%s\"", device.c_str()));

    if (m_state != IMPL_RUNNING) {
        QCC_DbgPrintf(("P2PConManImpl::DestroyTemporaryNetwork(): Not IMPL_RUNNING"));
        return ER_FAIL;
    }

    //
    // We only allow one thread at a time to be in here trying to make or
    // destroy a connection.  This means that the last thread to try and
    // establish or release a connnection will win.
    //
    m_establishLock.Lock(MUTEX_CONTEXT);

    //
    // We are really just doing a courtesy advisory to the P2P Helper Server since
    // Android allows anyone to walk over a temporary (Wi-Fi Direct) network and
    // delete it at any time.  We give up our references to it, so even if the
    // release doesn't work, we've forgotten it.  Since we blow away the handle
    // and set the state to CONN_IDLE, any callbacks that will happen as a result
    // of ReleaseLinkAsync will be tossed, but there's really nothing we can do
    // if the framework refuses to release a link if we tell it to, so we acutally
    // ignore all errors, but log an initial call failure if it happens.
    //
    int32_t handle = m_handle;
    m_handle = -1;
    m_device = "";
    m_interface = "";
    m_connState = CONN_IDLE;

    QCC_DbgPrintf(("P2PConManImpl::DestroyTemporaryNetwork(): ReleaseLinkAsync()"));
    QStatus status = m_p2pHelperInterface->ReleaseLinkAsync(handle);
    if (status != ER_OK) {
        QCC_LogError(status, ("P2PConManImpl::DestroyTemporaryNetwork(): ReleaseLinkAsync(): Call failure"));
    }

    m_establishLock.Unlock(MUTEX_CONTEXT);
    return ER_OK;
}

bool P2PConManImpl::IsConnected(const qcc::String& device)
{
    QCC_DbgHLPrintf(("P2PConManImpl::IsConnected(): \"%s\"", device.c_str()));

    //
    // We're actually being asked if we are connected to the given device, so
    // consider the device MAC address in the result.
    //
    if (m_state != IMPL_RUNNING) {
        QCC_DbgPrintf(("P2PConManImpl::IsConnected(): Not IMPL_RUNNING"));
        return ER_FAIL;
    }

    return m_state == IMPL_RUNNING && m_connState == CONN_CONNECTED && m_device == device;
}

QStatus P2PConManImpl::CreateConnectSpec(const qcc::String& device, const qcc::String& guid, qcc::String& spec)
{
    QCC_DbgHLPrintf(("P2PConManImpl::CreateConnectSpec(): \"%s\"/\"%s\"", device.c_str(), guid.c_str()));

    if (m_state != IMPL_RUNNING) {
        QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Not IMPL_RUNNING"));
        return ER_FAIL;
    }

    //
    // If we're going to use a network to run the IP name service over, we'd
    // better have one, at least to start.  Of course, this connection may
    // actually drop at any time, but we demand one at the outset.
    //
    if (m_connState != CONN_CONNECTED) {
        QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Not CONN_CONNECTED"));
        return ER_P2P_NOT_CONNECTED;
    }

    //
    // We only allow one thread at a time to be in here trying to figure out a
    // connect spec.  This whole process is like the layer three image of the
    // layer two CreateTemporaryNetwork process; and so the code is similar.
    //
    m_discoverLock.Lock(MUTEX_CONTEXT);

    //
    // Since we are now supposed to be the only thread dealing with layer three
    // addresses, we expect that a previous thread has cleaned up after itself.
    //
    assert(m_l3thread == NULL && "P2PConManImpl::CreateConnectSpec(): m_l3thread was left set");

    m_l3thread = qcc::Thread::GetThread();
    m_foundAdvertisedNameFired = false;
    m_busAddress = "";
    m_searchedGuid = guid;

    //
    // Tell the IP name service to call us back on our FoundAdvertisedName method when
    // it hears a response.
    //
    IpNameService::Instance().SetCallback(TRANSPORT_WFD,
                                          new CallbackImpl<P2PConManImpl, void, const qcc::String&, const qcc::String&, std::vector<qcc::String>&, uint8_t>
                                              (this, &P2PConManImpl::FoundAdvertisedName));

    //
    // Eventually, we are going to have to deal with the possibility of multiple
    // interfaces, but we know now that only one interface is possible and it is
    // going to be called "p2p0" on Android systems.  We also know that we are
    // doing this work on behalf of the Wi-Fi Direct transport.  Rather than
    // implement some more generic code now, we are going to make those
    // assumptions.
    //
    // In order to convince the IP name service to send packets out a P2P
    // interface, we must explicitly open that interface.
    //
    qcc::String interface("p2p0");
    QStatus status = IpNameService::Instance().OpenInterface(TRANSPORT_WFD, interface);

    //
    // We know there is a daemon out there that has advertised a service our
    // client found interesting.  The client decided to do a JoinSession to that
    // service which is what got us here.  We don't know the name of that
    // service, so we ask all of the daemons on the network if they have any
    // services.  All daemons (there will actually only be one) will respond
    // with all of their (its) services, which is what it would normally do,
    // so this request isn't actually unusual.
    //
    // What this does is to convince the remote daemon to give up its GUID and
    // all of the methods we can use to connect to it (IPv4 and IPv6 addresses,
    // reliable ports and unreliable ports).  We can then match the GUID in the
    // response to the GUID passed in as a parameter.  The device is there in
    // case of the possibility of multiple network connections, which is
    // currently unsupported.  We only support one network, so the device is
    // redundant and not currently used.
    //
    qcc::String star("*");
    QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): FindAdvertisedName()"));
    status = IpNameService::Instance().FindAdvertisedName(TRANSPORT_WFD, star);
    if (status != ER_OK) {
        m_discoverLock.Unlock(MUTEX_CONTEXT);
        QCC_LogError(status, ("P2PConManImpl::CreateConnectSpec(): FindAdvertisedName(): Failure"));
        return status;
    }

    qcc::Timespec tTimeout = CREATE_CONNECT_SPEC_TIMEOUT;
    qcc::Timespec tStart;
    qcc::GetTimeNow(&tStart);

    for (;;) {

        //
        // If the FoundAdvertisedName() callback fires and its handler determines
        // that the provided infomation matches our searchedGuid, then we have succeeded in
        // collecting enough information to construct our connect spec.
        //
        if (m_foundAdvertisedNameFired) {
            status = ER_OK;
            break;
        }

        //
        // Wait for something interesting to happen, but not too long.  Only
        // wait until a cummulative time from the starting time before declaring
        // a timeout.
        //
        qcc::Timespec tNow;
        qcc::GetTimeNow(&tNow);

        QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): tStart == %d", tStart.GetAbsoluteMillis()));
        QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): tTimeout == %d", tTimeout.GetAbsoluteMillis()));
        QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): tNow == %d", tNow.GetAbsoluteMillis()));

        if (tNow < tStart + tTimeout) {
            qcc::Timespec tWait = tStart + tTimeout - tNow;
            QCC_DbgPrintf(("P2PConManImpl::CreateTemporaryNetwork(): tWait == %d", tWait.GetAbsoluteMillis()));
            qcc::Event evt(tWait.GetAbsoluteMillis(), 0);
            QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Wait for something to happen"));
            status = qcc::Event::Wait(evt);
            QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Something happened"));

            //
            // We use Alert(PRIVATE_ALERT_CODE) in our callbacks to unblock the
            // thread from the Event::Wait() above if an interesting event
            // happens.  This causes the wait to return with ER_ALERTED_THREAD.
            // If we see this error, we look to see if the thread was alerted by
            // us.  This is the case if we see our private alert code.
            //
            // If it was not us precipitating the Alert(), we return an error
            // since someone else needs us to stop what we are doing.  In
            // particular, the system might be going down so we can't just hang
            // around here and arbitrarily keep that from happening.
            //
            // If it was us who caused the Alert(), we reset the stop event that
            // Alert() is using under the sheets and pop up to take a look around
            // and see what happened.
            //
            if (status == ER_ALERTED_THREAD) {
                QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Something happened: Alerted thread"));
                assert(m_l3thread != NULL && "P2PConManImpl::CreateConnectSpec(): m_l3thread must not be NULL");
                if (m_l3thread->GetAlertCode() == PRIVATE_ALERT_CODE) {
                    m_l3thread->GetStopEvent().ResetEvent();
                    status = ER_OK;
                } else {
                    QCC_LogError(status, ("P2PConManImpl::CreateConnectSpec(): Thread has been Alert()ed"));
                    break;
                }
            }
        } else {
            status = ER_P2P_TIMEOUT;
            QCC_LogError(status, ("P2PConManImpl::CreateConnectSpec(): Timeout"));
            break;
        }
    }

    QCC_DbgPrintf(("P2PConManImpl::CreateConnectSpec(): Out of loop.  Status = %s", QCC_StatusText(status)));

    //
    // The thread that called to start this whole deal is returning to what
    // fate we do not know.  We don't want to affect it any more, so we
    // need to forget about it.  But before we send it on its way, we need
    // to make sure that we don't leave the thread alerted.  Bad things can
    // happen if the calling thread doesn't expect this.
    //
    m_threadLock.Lock();
    if (m_l3thread->GetAlertCode() == PRIVATE_ALERT_CODE) {
        m_l3thread->GetStopEvent().ResetEvent();
        m_l3thread->ResetAlertCode();
    }
    m_l3thread = NULL;
    m_threadLock.Unlock();

    //
    // If we succeeded, the IP name service has done our work for us and
    // provided us with a bus address that has all of the connect information
    // in it.
    //
    if (status == ER_OK) {
        spec = m_busAddress;
    } else {
        spec = "";
    }

    //
    // Not strictly required, but tell the IP name service that it can
    // forget about the name in question.
    //
    IpNameService::Instance().CancelFindAdvertisedName(TRANSPORT_WFD, star);

    //
    // Don't leave the IP name service trying to transmit and receive over the
    // P2P interface.  It's wasted energy now.  Since we are relying on Wi-Fi
    // Direct, for our advertisements, we rely on it completely.
    //
    IpNameService::Instance().CloseInterface(TRANSPORT_WFD, interface);

    //
    // Tell the IP name service to forget about calling us back.
    //
    IpNameService::Instance().SetCallback(TRANSPORT_WFD, NULL);

    m_discoverLock.Unlock(MUTEX_CONTEXT);
    return status;
}

void P2PConManImpl::OnLinkEstablished(int32_t handle, qcc::String& interface)
{
    QCC_DbgHLPrintf(("P2PConManImpl::OnLinkEstablished(): handle = %d, interface=\"%s\"", handle, interface.c_str()));

    if (m_connState != CONN_CONNECTING && m_connState != CONN_READY) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkEstablished(): Not CONN_CONNECTING or CONN_READY"));
        return;
    }

    //
    // If we are in the CONN_READY state, we are the service side of the
    // equation.  We have told the framework that we are the service side, but
    // we don't have a handle back from it since there was no connection -- we
    // just advised the framework that we were there.  The
    // CreateTemporaryNetwork call is long gone so we don't communicate back to
    // it, we just need to make a note to ourselves that a connection is up, and
    // remember the handle describing the connection, and the interface name of
    // the net device that was just brought up.  Since we don't know anything
    // about our own devices, we leave the address of the P2P Device empty.
    //
    if (m_connState == CONN_READY) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkEstablished(): OnLinkEstablished for GO"));
        m_handle = handle;
        m_interface = interface;
        m_connState = CONN_CONNECTED;
        m_device = "";

        //
        // Call back any interested parties (transports) and tell them that a
        // link has been established and let them know which network interface
        // is handling the link.
        //
        if (m_callback) {
            (*m_callback)(P2PConMan::ESTABLISHED, m_interface);
        }
        return;
    }

    //
    // We need to make sure that the OnLinkEstablished() callback we are getting
    // is coherent with the EstablishLinkAsync() we think we are working on.  We
    // do this via the handle.
    //
    if (m_handle == handle) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkEstablished(): OnLinkEstablished with correct handle"));
        m_onLinkEstablishedFired = true;

        //
        // We don't know which net device (interface name) is going to be handling
        // the connection until the link is actually brought up.
        //
        m_interface = interface;

        //
        // Call back any interested parties (transports) and tell them that a
        // link has been established and let them know which network interface
        // is handling the link.
        //
        if (m_callback) {
            (*m_callback)(P2PConMan::ESTABLISHED, m_interface);
        }

        m_threadLock.Lock();
        if (m_l2thread) {
            QCC_DbgPrintf(("P2PConManImpl::OnLinkEstablished(): Alert() blocked thread"));
            m_l2thread->Alert(PRIVATE_ALERT_CODE);
        }
        m_threadLock.Unlock();
    }
}

void P2PConManImpl::OnLinkError(int32_t handle, int32_t error)
{
    QCC_DbgHLPrintf(("P2PConManImpl::OnLinkError(): handle = %d, error = %d", handle, error));

    if (m_connState != CONN_CONNECTING) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkError(): Not CONN_CONNECTING"));
        return;
    }

    //
    // We need to make sure that the OnLinkError() callback we are getting is
    // coherent with the EstablishLinkAsync() we think we are working on.  We do
    // this via the handle.
    //
    if (m_handle == handle) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkError(): OnLinkError with correct handle"));
        m_linkError = error;
        m_onLinkErrorFired = true;

        m_threadLock.Lock();
        if (m_l2thread) {
            QCC_DbgPrintf(("P2PConManImpl::OnLinkEstablished(): Alert() blocked thread"));
            m_l2thread->Alert(PRIVATE_ALERT_CODE);
        }
        m_threadLock.Unlock();
    }
}

void P2PConManImpl::OnLinkLost(int32_t handle)
{
    QCC_DbgHLPrintf(("P2PConManImpl::OnLinkLost(): handle = %d", handle));

    //
    // If we get an OnLinkLost we need to make sure it is for a link we
    // think is up.  If we get a stale OnLinkLost() for a link we may have
    // just killed, we need to make sure we ignore it.
    //
    if (m_handle == handle) {
        QCC_DbgPrintf(("P2PConManImpl::OnLinkLost(): OnLinkLost with correct handle.  Connection is dead."));

        //
        // Call back any interested parties (transports) and tell them that a
        // link has been established and let them know which network interface
        // is handling the link.  Make this call before we clear the interface
        // name since the trasports probably want to use it for cleanup.
        //
        if (m_callback) {
            (*m_callback)(P2PConMan::LOST, m_interface);
        }

        m_handle = -1;
        m_device = "";
        m_interface = "";
        m_connState = CONN_IDLE;

        m_threadLock.Lock();
        if (m_l2thread) {
            QCC_DbgPrintf(("P2PConManImpl::OnLinkLost(): Alert() blocked thread"));
            m_l2thread->Alert(PRIVATE_ALERT_CODE);
        }
        m_threadLock.Unlock();
    }
}

void P2PConManImpl::HandleEstablishLinkReply(int32_t handle)
{
    QCC_DbgHLPrintf(("P2PConManImpl::HandleEstablishLinkReply(): handle = %d", handle));

    if (m_connState != CONN_CONNECTING) {
        QCC_DbgPrintf(("P2PConManImpl::HandleEstablishLinkReply(): Not CONN_CONNECTING"));
        return;
    }

    //
    // HandleEstablishLinkReply is the response to EstablishLinkAsync that gives
    // us the handle that we will be using to identify all further responses.  A
    // negative handle means an error.
    //
    // XXX We have some more possibilites for error returns now than simple
    // failure.
    //
    m_handle = handle;
    if (m_handle < 0) {
        m_establishLinkResult = P2PHelperInterface::P2P_ERR;
    } else {
        m_establishLinkResult = P2PHelperInterface::P2P_OK;
    }

    m_handleEstablishLinkReplyFired = true;

    m_threadLock.Lock();
    if (m_l2thread) {
        QCC_DbgPrintf(("P2PConManImpl::HandleEstablishLinkReply(): Alert() blocked thread"));
        m_l2thread->Alert(PRIVATE_ALERT_CODE);
    }
    m_threadLock.Unlock();
}

void P2PConManImpl::HandleReleaseLinkReply(int32_t result)
{
    //
    // If we can't actually convince the P2P Helper Service, or the Android
    // Application Framework to release our link, there's really nothing we can
    // do.  We just print a debug message in case someone is watching who might
    // care.
    //
    QCC_DbgHLPrintf(("P2PConManImpl::HandleReleaseLinkReply(): result = %d", result));
}

void P2PConManImpl::HandleGetInterfaceNameFromHandleReply(qcc::String& interface)
{
    //
    // Historical and currently unused.
    //
    QCC_DbgHLPrintf(("P2PConManImpl::HandleGetInterfacenameFromHandleReply(): interface = \"%d\"", interface.c_str()));
}

void P2PConManImpl::FoundAdvertisedName(const qcc::String& busAddr, const qcc::String& guid,
                                        std::vector<qcc::String>& nameList, uint8_t timer)
{
    QCC_DbgPrintf(("P2PConManImpl::FoundAdvertisedName(): busAddr = \"%s\", guid = \"%s\"", busAddr.c_str(), guid.c_str()));

    //
    // If the guid of the remote daemon matches the guid that we are searching for,
    // we have our addressing information.  It is in the provided busAddr.
    //
    if (m_searchedGuid == guid) {
        m_busAddress = busAddr;
        m_foundAdvertisedNameFired = true;
        m_threadLock.Lock();
        if (m_l3thread) {
            m_l3thread->Alert(PRIVATE_ALERT_CODE);
        }
        m_threadLock.Unlock();
    }
}

} // namespace ajn
