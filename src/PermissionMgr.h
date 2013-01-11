/**
 * @file
 * This class is to manage the permission of an endpoint on using transports or invoking method/signal calls on another peer.
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
#ifndef _PERMISSION_MGR_H
#define _PERMISSION_MGR_H

#include "LocalTransport.h"
#include "TransportList.h"
#include <qcc/ThreadPool.h>

namespace ajn {

#define MAX_PERM_CHECKEDCALL_SIZE (512)

class TransportPermission {
  public:
    /**
     * Filter out transports that the endpoint has no permissions to use
     * @param   srcEp         The source endpoint
     * @param   sender        The sender's well-known name string
     * @param   transports    The transport mask
     * @param   callerName    The caller that invokes this method
     */
    static QStatus FilterTransports(BusEndpoint& srcEp, const qcc::String& sender, TransportMask& transports, const char* callerName);

    /**
     * Get transports that the endpoint has no permission to use
     * @param   uid             Uid to check.
     * @param   transList       List of transports available
     * @param   transForbidden  Mask of fobidden transports
     * @param   callerName      The caller that invokes this method
     */
    static void GetForbiddenTransports(uint32_t uid, TransportList& transList, TransportMask& transForbidden, const char* callerName);
};

class PermissionMgr {
  public:
    typedef enum {
        STDBUSCALL_ALLOW_ACCESS_SERVICE_ANY = 0,         /**< A standard daemon bus call is allowed to interact with any local or remote service */
        STDBUSCALL_ALLOW_ACCESS_SERVICE_LOCAL = 1,       /**< A standard daemon bus call is allowed, but it can only interact with local service */
        STDBUSCALL_SHOULD_REJECT = 2                     /**< A standard daemon bus call should always be rejected */
    } DaemonBusCallPolicy;

    /**
     * Add an alias ID to a UnixEndpoint User ID
     * @param srcEp     The source endpoint
     * @param origUID   The unique User ID
     * @param aliasUID  The alias User ID
     */
    static uint32_t AddAliasUnixUser(BusEndpoint& srcEp, qcc::String& sender, uint32_t origUID, uint32_t aliasUID);

    /**
     * Cleanup the permission information cache of an enpoint before it exits.
     */
    static QStatus CleanPermissionCache(BusEndpoint& endpoint);

    /**
     * Get the policy for a bus endpoint's permission of invoking the standard DBus and AllJoyn inteface.
     * @param   endpoint   The bus endpoint.
     * @return  the policy associated with the bus endpoint for invoking the standard DBus and AllJoyn inteface.
     */
    static DaemonBusCallPolicy GetDaemonBusCallPolicy(BusEndpoint sender);

    /**
     * Check whether the bus endpoint is already authenticated
     */
    static bool IsEndpointAuthorized(BusEndpoint sender);
};

} // namespace ajn

#endif //_PERMISSION_MGR_H
