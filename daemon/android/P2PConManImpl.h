/**
 * @file
 * Implementation of the AllJoyn Android Wi-Fi Direct (Wi-Fi P2P) connection manager
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

#ifndef _P2P_CON_MAN_IMPLH
#define _P2P_CON_MAN_IMPLH

#ifndef __cplusplus
#error Only include P2PConManImpl.h in C++ code.
#endif

#include <vector>
#include <qcc/String.h>
#include <Status.h>
#include "P2PHelperInterface.h"

namespace ajn {

/**
 * @brief API to provide an implementation dependent P2P (Layer 2) Connection
 * Manager for AllJoyn.
 */
class P2PConManImpl {
  public:
    /**
     * @brief Construct a P2P connection manager implementation object.
     */
    P2PConManImpl();

    /**
     * @brief Destroy a P2P connection manager implementation object.
     */
    virtual ~P2PConManImpl();

    /**
     * @brief Initialize the P2PConManImpl.
     *
     * @param guid A string containing the GUID assigned to the daemon which is
     *     hosting the connection manager.
     */
    QStatus Init(BusAttachment* bus, const qcc::String& guid);

    /**
     * @brief Stop any name service threads.
     *
     * We don't have any threads here, but it may be the case that one of the objects we use
     * does.  Currently does nothing.
     */
    QStatus Start() { m_state = IMPL_RUNNING; return ER_OK; }

    /**
     * @brief Determine if the connection manager has been started.
     *
     * @return True if the connection manager has been started, false otherwise.
     */
    bool Started() { return m_state == IMPL_RUNNING; }

    /**
     * @brief Stop any name service threads.
     *
     * We don't have any threads here, but it may be the case that one of the objects we use
     * does.  Currently does nothing.
     */
    QStatus Stop() { m_state = IMPL_STOPPING; return ER_OK; }

    /**
     * @brief Join any name service threads.
     *
     * We don't have any threads here, but it may be the case that one of the objects we use
     * does.  Currently does nothing.
     */
    QStatus Join() { m_state = IMPL_SHUTDOWN; return ER_OK; }

    /**
     * @brief Create a temporary physical network connection to the provided
     *     device MAC address using Wi-Fi Direct.
     *
     * @param[in] device The MAC address of the remote device presented as a string.
     * @param[in] intent The Wi-Fi Direcct group owner intent value.
     *
     * @return ER_OK if the network is successfully created, otherwise (hopefully)
     *     appropriate error code reflecting outcome.
     */
    QStatus CreateTemporaryNetwork(const qcc::String& device, uint32_t intent);

    /**
     * @brief Destroy a temporary physical network connection to the provided
     *     device MAC address.
     *
     * @param[in] device The MAC address of the remote device presented as a string.
     * @param[in] intent The Wi-Fi Direcct group owner intent value.
     *
     * @return ER_OK if the network is successfully created, otherwise (hopefully)
     *     appropriate error code reflecting outcome.
     */
    QStatus DestroyTemporaryNetwork(const qcc::String& device, uint32_t intent);

    /**
     * @brief Determine if the P2PConman knows about a connection to the device
     *     with the given MAC address
     *
     * @param[in] device The MAC address of the remote device presented as a string.
     *
     * @return True if a physical network has been created that allows us to
     *     access <device>.
     */
    bool IsConnected(const qcc::String& device);

    /**
     * @brief Return an appropriate connect spec <spec> for use in making a TCP
     *     to a daemon specified by <guid> that is running on the device with
     *     MAC address specified by <device>.
     *
     * @param[in]  device The MAC address of the remote device presented as a string.
     * @param[in]  guid The GUID of the remote daemon presented as a string.
     * @param[out] spec A connect spec that can be used to connect to the remote
     *     daemon.
     *
     * @return ER_OK if the connect spec can be determined.
     */
    QStatus CreateConnectSpec(const qcc::String& device, const qcc::String& guid, qcc::String& spec);

  private:
    /**
     * @brief Copying an IpConManImpl object is forbidden.
     */
    P2PConManImpl(const P2PConManImpl& other);

    /**
     * @brief Assigning a P2PConManImpl object is forbidden.
     */
    P2PConManImpl& operator =(const P2PConManImpl& other);

    /**
     * @brief
     * Private notion of what state the implementation object is in.
     */
    enum State {
        IMPL_INVALID,           /**< Should never be seen on a constructed object */
        IMPL_SHUTDOWN,          /**< Nothing is running and object may be destroyed */
        IMPL_INITIALIZING,      /**< Object is in the process of coming up and may be inconsistent */
        IMPL_RUNNING,           /**< Object is running and ready to go */
        IMPL_STOPPING,          /**< Object is stopping */
    };

    /**
     * @brief State variable to indicate what the implementation is doing or is
     * capable of doing.
     */
    State m_state;

    /**
     * @brief The daemon GUID string of the daemon associated with this instance
     * of the name service.
     */
    qcc::String m_guid;

    void OnFoundAdvertisedName(qcc::String& name, qcc::String& namePrefix, qcc::String& guid, qcc::String& device) { }
    void OnLostAdvertisedName(qcc::String& name, qcc::String& namePrefix, qcc::String& guid, qcc::String& device) { }
    void OnLinkEstablished(int32_t handle) { }
    void OnLinkError(int32_t handle, int32_t error) { }
    void OnLinkLost(int32_t handle) { }
    void HandleFindAdvertisedNameReply(int32_t result) { }
    void HandleCancelFindAdvertisedNameReply(int32_t result) { }
    void HandleAdvertiseNameReply(int32_t result) { }
    void HandleCancelAdvertiseNameReply(int32_t result) { }
    void HandleEstablishLinkReply(int32_t handle) { }
    void HandleReleaseLinkReply(int32_t result) { }
    void HandleGetInterfaceNameFromHandleReply(qcc::String& interface) { }

    /**
     * A listener class to receive events from an underlying Wi-Fi Direct
     * helper service.  The helper actually talks to an AllJoyn service
     * which, in turn, talks to the Android Application Framework.  Events
     * from the framework are sent back to the helper as AllJoyn signals
     * which then find their way to this listener class.  We just forward
     * them on back to the P2PConMan which digests them.
     */
    class MyP2PHelperListener : public P2PHelperListener {
      public:
        MyP2PHelperListener(P2PConManImpl* cmi) : m_cmi(cmi) { }
        ~MyP2PHelperListener() { }

        virtual void OnFoundAdvertisedName(qcc::String& name, qcc::String& namePrefix, qcc::String& guid, qcc::String& device)
        {
            assert(m_cmi);
            m_cmi->OnFoundAdvertisedName(name, namePrefix, guid, device);
        }

        virtual void OnLostAdvertisedName(qcc::String& name, qcc::String& namePrefix, qcc::String& guid, qcc::String& device)
        {
            assert(m_cmi);
            m_cmi->OnLostAdvertisedName(name, namePrefix, guid, device);
        }

        virtual void OnLinkEstablished(int32_t handle)
        {
            assert(m_cmi);
            m_cmi->OnLinkEstablished(handle);
        }

        virtual void OnLinkError(int32_t handle, int32_t error)
        {
            assert(m_cmi);
            m_cmi->OnLinkError(handle, error);
        }

        virtual void OnLinkLost(int32_t handle)
        {
            assert(m_cmi);
            m_cmi->OnLinkLost(handle);
        }

        virtual void HandleFindAdvertisedNameReply(int32_t result)
        {
            assert(m_cmi);
            m_cmi->HandleFindAdvertisedNameReply(result);
        }

        virtual void HandleCancelFindAdvertisedNameReply(int32_t result)
        {
            assert(m_cmi);
            m_cmi->HandleCancelFindAdvertisedNameReply(result);
        }

        virtual void HandleAdvertiseNameReply(int32_t result)
        {
            assert(m_cmi);
            m_cmi->HandleAdvertiseNameReply(result);
        }

        virtual void HandleCancelAdvertiseNameReply(int32_t result)
        {
            assert(m_cmi);
            m_cmi->HandleCancelAdvertiseNameReply(result);
        }

        virtual void HandleEstablishLinkReply(int32_t handle)
        {
            assert(m_cmi);
            m_cmi->HandleEstablishLinkReply(handle);
        }

        virtual void HandleReleaseLinkReply(int32_t result)
        {
            assert(m_cmi);
            m_cmi->HandleReleaseLinkReply(result);
        }

        virtual void HandleGetInterfaceNameFromHandleReply(qcc::String& interface)
        {
            assert(m_cmi);
            m_cmi->HandleGetInterfaceNameFromHandleReply(interface);
        }

      private:
        P2PConManImpl* m_cmi;
    };

    MyP2PHelperListener* m_myP2pHelperListener; /**< The listener that receives events from the P2P Helper Service */
    P2PHelperInterface* m_p2pHelperInterface;   /**< The AllJoyn interface used to talk to the P2P Helper Service */

    BusAttachment* m_bus;                       /**< The AllJoyn bus attachment that we use to talk to the P2P Helper Service */
};

} // namespace ajn

#endif // _P2P_CON_MAN_IMPL_H
