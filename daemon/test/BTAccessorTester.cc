/**
 * @file
 * AllJoyn service that implements interfaces and members affected test.conf.
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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

#include <list>
#include <set>

#include <qcc/GUID.h>
#include <qcc/Logger.h>
#include <qcc/Mutex.h>
#include <qcc/Stream.h>
#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/time.h>

#include "BDAddress.h"
#include "Transport.h"


using namespace std;
using namespace qcc;


namespace ajn {

/********************
 * TEST STUBS
 ********************/

#define _ALLJOYNBTTRANSPORT_H
class BTTransport {

  public:
    class BTAccessor;

    std::set<RemoteEndpoint*> threadList;
    Mutex threadListLock;

    BTTransport() { }
    virtual ~BTTransport() { }

    void BTDeviceAvailable(bool avail);
    bool CheckIncomingAddress(const BDAddress& addr) const;
    void DisconnectAll();

  protected:
    virtual void TestBTDeviceAvailable(bool avail) = 0;
    virtual bool TestCheckIncomingAddress(const BDAddress& addr) const = 0;
    virtual void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable) = 0;

  private:
    void DeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

};


void BTTransport::BTDeviceAvailable(bool avail)
{
    TestBTDeviceAvailable(avail);
}
bool BTTransport::CheckIncomingAddress(const BDAddress& addr) const
{
    return TestCheckIncomingAddress(addr);
}
void BTTransport::DeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable)
{
    TestDeviceChange(bdAddr, uuidRev, eirCapable);
}
void BTTransport::DisconnectAll()
{
}

}


#include "BTNodeDB.h"
#include "BTNodeInfo.h"

#if defined QCC_OS_GROUP_POSIX
#if defined(QCC_OS_DARWIN)
#error Darwin support for bluetooth to be implemented
#else
#include "../bt_bluez/BTAccessor.h"
#endif
#elif defined QCC_OS_GROUP_WINDOWS
#include "../bt_windows/BTAccessor.h"
#endif


#define TestCaseFunction(_tcf) static_cast<TestDriver::TestCase>(&_tcf)


using namespace ajn;

void XorByteArray(const uint8_t* in1, const uint8_t* in2, uint8_t* out, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        out[i] = in1[i] ^ in2[i];
    }
}


struct CmdLineOptions {
    String basename;
    bool client;
    bool server;
    bool allowInteractive;
    bool reportDetails;
    bool local;
    bool fastDiscovery;
    bool quiet;
    CmdLineOptions() :
        basename("org.alljoyn.BTAccessorTester"),
        client(false),
        server(false),
        allowInteractive(true),
        reportDetails(false),
        local(false),
        fastDiscovery(false),
        quiet(false)
    {
    }
};

class TestDriver : public BTTransport {
  public:
    typedef bool (TestDriver::*TestCase)();

    struct TestCaseInfo {
        TestDriver::TestCase tc;
        String description;
        bool success;
        TestCaseInfo(TestDriver::TestCase tc, const String& description) :
            tc(tc), description(description), success(false)
        {
        }
    };

    struct DeviceChange {
        BDAddress addr;
        uint32_t uuidRev;
        bool eirCapable;
        DeviceChange(const BDAddress& addr, uint32_t uuidRev, bool eirCapable) :
            addr(addr), uuidRev(uuidRev), eirCapable(eirCapable)
        {
        }
    };


    TestDriver(const CmdLineOptions& opts);
    virtual ~TestDriver();

    void AddTestCase(TestDriver::TestCase tc, const String description);
    int RunTests();

    bool TC_CreateBTAccessor();
    bool TC_DestroyBTAccessor();
    bool TC_StartBTAccessor();
    bool TC_StopBTAccessor();
    bool TC_IsMaster();
    bool TC_RequestBTRole();
    bool TC_IsEIRCapable();
    bool TC_StartConnectable();
    bool TC_StopConnectable();

  protected:
    BTAccessor* btAccessor;
    BusAttachment bus;
    const CmdLineOptions& opts;
    qcc::GUID busGuid;
    RemoteEndpoint* ep;

    deque<bool> btDevAvailQueue;
    Event btDevAvailEvent;
    Mutex btDevAvailLock;

    deque<DeviceChange> devChangeQueue;
    Event devChangeEvent;
    Mutex devChangeLock;

    set<BDAddress> connectedDevices;

    bool eirCapable;
    BTNodeInfo self;
    BTNodeDB nodeDB;

    void TestBTDeviceAvailable(bool available);
    virtual bool TestCheckIncomingAddress(const BDAddress& addr) const;
    virtual void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    void ReportTestDetail(const String detail) const;
    bool SendBuf(const uint8_t* buf, size_t size);
    bool RecvBuf(uint8_t* buf, size_t size);


  private:
    list<TestCaseInfo> tcList;
    uint32_t testcase;
    mutable list<String> detailList;
    bool success;
    list<TestCaseInfo>::iterator insertPos;
    size_t maxWidth;
    size_t tcNumWidth;
    static const size_t tcWidth = 2;
    static const size_t tcColonWidth = 2;
    static const size_t pfWidth = 5;

    void RunTest(TestCaseInfo& test);
};


class ClientTestDriver : public TestDriver {
  public:
    struct FoundInfo {
        uint32_t found;
        uint32_t changed;
        uint32_t uuidRev;
        bool checked;
        FoundInfo() :
            found(0), changed(0), uuidRev(0), checked(false)
        {
        }
        FoundInfo(uint32_t uuidRev) :
            found(1), changed(0), uuidRev(uuidRev), checked(false)
        {
        }
    };

    ClientTestDriver(const CmdLineOptions& opts);
    virtual ~ClientTestDriver() { }

    bool TestCheckIncomingAddress(const BDAddress& addr) const;
    void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    bool TC_StartDiscovery();
    bool TC_StopDiscovery();
    bool TC_GetDeviceInfo();
    bool TC_ConnectSingle();
    bool TC_ConnectMultiple();
    bool TC_ExchangeSmallData();
    bool TC_ExchangeLargeData();
    bool TC_IsMaster();
    bool TC_RequestBTRole();

  private:
    map<BDAddress, FoundInfo> foundInfo;
    uint32_t connUUIDRev;
    BTBusAddress connAddr;
    BTNodeDB connAdInfo;
    BTNodeInfo connNode;

    bool ExchangeData(size_t size);
};


class ServerTestDriver : public TestDriver {
  public:
    ServerTestDriver(const CmdLineOptions& opts);
    virtual ~ServerTestDriver() { }

    bool TestCheckIncomingAddress(const BDAddress& addr) const;
    void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    bool TC_StartDiscoverability();
    bool TC_StopDiscoverability();
    bool TC_SetSDPInfo();
    bool TC_GetL2CAPConnectEvent();
    bool TC_AcceptSingle();
    bool TC_AcceptMultiple();
    bool TC_ExchangeSmallData();
    bool TC_ExchangeLargeData();

  private:
    bool allowIncomingAddress;
    uint32_t uuidRev;

    bool ExchangeData(size_t size);
};


TestDriver::TestDriver(const CmdLineOptions& opts) :
    btAccessor(NULL),
    bus("BTAccessorTester"),
    opts(opts),
    ep(NULL), 
    testcase(0),
    success(true),
    maxWidth(80),
    tcNumWidth(2)
{
    String uniqueName = ":";
    uniqueName += busGuid.ToShortString();
    uniqueName += ".1";
    self->SetGUID(busGuid);
    self->SetRelationship(_BTNodeInfo::SELF);
    self->SetUniqueName(uniqueName);

    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_CreateBTAccessor), "Create BT Accessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StartBTAccessor), "Start BTAccessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_IsEIRCapable), "Check EIR capability"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StartConnectable), "Start Connectable"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StopConnectable), "Stop Connectable"));
    insertPos = tcList.end();
    --insertPos;
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StopBTAccessor), "Stop BTAccessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_DestroyBTAccessor), "Destroy BTAccessor"));
}

TestDriver::~TestDriver()
{
    if (ep) {
        delete ep;
    }
    if (btAccessor) {
        delete btAccessor;
    }
}

void TestDriver::AddTestCase(TestDriver::TestCase tc, const String description)
{
    tcList.insert(insertPos, TestCaseInfo(tc, description));
    tcNumWidth = 1 + ((tcList.size() > 100) ? 3 :
                      ((tcList.size() > 10) ? 2 : 1));
    if ((tcWidth + tcNumWidth + 1 + description.size() + tcColonWidth + pfWidth) > (maxWidth)) {
        maxWidth = (tcWidth + tcNumWidth + description.size() + tcColonWidth + pfWidth);
    }
}

int TestDriver::RunTests()
{
    list<TestCaseInfo>::iterator it;
    for (it = tcList.begin(); success && (it != tcList.end()); ++it) {
        TestCaseInfo& test = *it;
        RunTest(test);
    }

    return success ? 0 : 1;
}

void TestDriver::RunTest(TestCaseInfo& test)
{
    static const size_t dashWidth = 2;
    const size_t detailIndent = (4 + tcWidth + tcNumWidth + 1);
    const size_t detailWidth = maxWidth - (detailIndent + dashWidth);

    size_t i;
    String line;
    line.reserve(maxWidth);

    line.append("TC");
    line.append(U32ToString(++testcase, 10, tcNumWidth, ' '));
    line.push_back(' ');
    line.append(test.description);
    line.append(": ");

    while (line.size() < (maxWidth - (pfWidth + 1))) {
        line.push_back('.');
    }

    printf("%s", line.c_str());
    fflush(stdout);

    test.success = (this->*(test.tc))();

    line.clear();

    line.append(test.success ? " PASS" : " FAIL");

    printf("%s\n", line.c_str());
    line.clear();

    while (detailList.begin() != detailList.end()) {
        String detail = detailList.front();
        if (!detail.empty()) {
            bool wrapped = false;
            while (!detail.empty()) {
                for (i = 0; i < detailIndent; ++i) {
                    line.push_back(' ');
                }
                line.append(wrapped ? "  " : "- ");

                if (detail.size() > detailWidth) {
                    String subline = detail.substr(0, detail.find_last_of(' ', detailWidth));
                    detail = detail.substr(detail.find_first_not_of(" ", subline.size()));
                    line.append(subline);
                } else {
                    line.append(detail);
                    detail.clear();
                }
                printf("%s\n", line.c_str());
                line.clear();
                wrapped = !detail.empty();
            }
        }
        detailList.erase(detailList.begin());
    }
    success = success && test.success;
}

void TestDriver::ReportTestDetail(const String detail) const
{
    if (opts.reportDetails) {
        detailList.push_back(detail);
    }
}

bool TestDriver::SendBuf(const uint8_t* buf, size_t size)
{
    QStatus status;
    size_t offset = 0;
    size_t sent = 0;

    while (size > 0) {
        status = ep->GetSink().PushBytes(buf + offset, size, sent);
        if (status != ER_OK) {
            String detail = "Sending ";
            detail += U32ToString(size);
            detail += " bytes failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            return false;
        }

        offset += sent;
        size -= sent;
    }
    return true;
}

bool TestDriver::RecvBuf(uint8_t* buf, size_t size)
{
    QStatus status;
    size_t offset = 0;
    size_t received = 0;

    while (size > 0) {
        status = ep->GetSource().PullBytes(buf + offset, size, received, 30000);
        if (status != ER_OK) {
            String detail = "Receiving ";
            detail += U32ToString(size);
            detail += " bytes failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            return false;
        }
        offset += received;
        size -= received;
    }
    return true;
}

void TestDriver::TestBTDeviceAvailable(bool available)
{
    String detail = "Received device ";
    detail += available ? "available" : "unavailable";
    detail += " indication from BTAccessor.";
    ReportTestDetail(detail);
    btDevAvailLock.Lock();
    btDevAvailQueue.push_back(available);
    btDevAvailLock.Unlock();
    btDevAvailEvent.SetEvent();
}

bool TestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked: REJECTED (base test driver).";
    ReportTestDetail(detail);

    return false;
}

bool ClientTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked: REJECTED (client test driver).";
    ReportTestDetail(detail);

    return false;
}

bool ServerTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked: ";
    detail += allowIncomingAddress ? "allowed." : "rejected.";
    ReportTestDetail(detail);

    return allowIncomingAddress;
}

void TestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    ReportTestDetail("BTAccessor reported a found device to us.  Ignoring since this is the base Test Driver.");
}

void ClientTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    String detail = "BTAccessor reported a found device to us: ";
    detail += bdAddr.ToString().c_str();
    if (eirCapable) {
        detail += ".  It is EIR capable with a UUID revision of 0x";
        detail += U32ToString(uuidRev, 16, 8, '0');
        detail += ".";
    } else {
        detail += ".  It is not EIR capable.";
    }
    ReportTestDetail(detail);

    devChangeLock.Lock();
    devChangeQueue.push_back(DeviceChange(bdAddr, uuidRev, eirCapable));
    devChangeLock.Unlock();
    devChangeEvent.SetEvent();
}

void ServerTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    ReportTestDetail("BTAccessor reported a found device to us.  Ignoring since this is the Server Test Driver.");
}


/****************************************/

bool TestDriver::TC_CreateBTAccessor()
{
    btAccessor = new BTAccessor(this, busGuid.ToString());

    return true;
}

bool TestDriver::TC_DestroyBTAccessor()
{
    delete btAccessor;
    btAccessor = NULL;
    return true;
}

bool TestDriver::TC_StartBTAccessor()
{
    bool available = false;

    btDevAvailLock.Lock();
    btDevAvailQueue.clear();
    btDevAvailLock.Unlock();
    btDevAvailEvent.ResetEvent();

    QStatus status = btAccessor->Start();
    if (status != ER_OK) {
        String detail = "Call to start BT device failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        goto exit;
    }

    do {
        status = Event::Wait(btDevAvailEvent, 30000);
        if (status != ER_OK) {
            String detail = "Waiting for BT device available notification failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        btDevAvailLock.Lock();
        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }
        btDevAvailLock.Unlock();

        if (!available) {
            fprintf(stderr, "Please enable system's Bluetooth.\n");
        }
    } while (!available);

exit:
    return (status == ER_OK);
}

bool TestDriver::TC_StopBTAccessor()
{
    bool available = true;
    QStatus status = ER_OK;

    btAccessor->Stop();

    do {
        status = Event::Wait(btDevAvailEvent, 30000);
        if (status != ER_OK) {
            String detail = "Waiting for BT device available notification failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        btDevAvailLock.Lock();
        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }
        btDevAvailLock.Unlock();
    } while (available);

exit:
    return (status == ER_OK);
}

bool TestDriver::TC_IsMaster()
{
    bool tcSuccess = true;
    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        bool master;
        String detail;
        QStatus status = btAccessor->IsMaster(*it, master);
        if (status == ER_OK) {
            detail = "Got the ";
            detail += master ? "master" : "slave";
        } else {
            detail = "Failed to get master/slave";
            tcSuccess = false;
            goto exit;
        }
        detail += " role for connection with ";
        detail += it->ToString().c_str();
        if (status != ER_OK) {
            detail += ": ";
            detail += QCC_StatusText(status);
        }
        detail += ".";
        ReportTestDetail(detail);
    }

exit:
    return tcSuccess;
}

bool TestDriver::TC_RequestBTRole()
{
    bool tcSuccess = true;
    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        bool master;
        String detail;
        QStatus status = btAccessor->IsMaster(*it, master);
        if (status != ER_OK) {
            detail = "Failed to get master/slave role with ";
            detail += it->ToString().c_str();
            detail += ": ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        detail = "Switching role with ";
        detail += it->ToString().c_str();
        detail += master ? " to slave" : " to master";
        detail += ".";
        ReportTestDetail(detail);

        bt::BluetoothRole role = master ? bt::SLAVE : bt::MASTER;
        btAccessor->RequestBTRole(*it, role);

        status = btAccessor->IsMaster(*it, master);
        if (status != ER_OK) {
            detail = "Failed to get master/slave role with ";
            detail += it->ToString().c_str();
            detail += ": ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        if (master != (role == bt::MASTER)) {
            detail = "Failed to switch role with ";
            detail += it->ToString().c_str();
            detail += " (not a test case failure).";
            ReportTestDetail(detail);
        }

        detail = "Switching role with ";
        detail += it->ToString().c_str();
        detail += " back to ";
        detail += (role == bt::SLAVE) ? "master" : "slave";
        detail += ".";
        ReportTestDetail(detail);
        role = (role == bt::SLAVE) ? bt::MASTER : bt::SLAVE;
        btAccessor->RequestBTRole(*it, role);
    }

exit:
    return tcSuccess;
}

bool TestDriver::TC_IsEIRCapable()
{
    eirCapable = btAccessor->IsEIRCapable();
    self->SetEIRCapable(eirCapable);
    String detail = "The local device is ";
    detail += eirCapable ? "EIR capable" : "not EIR capable";
    detail += ".";
    ReportTestDetail(detail);
    return true;
}

bool TestDriver::TC_StartConnectable()
{
    QStatus status;
    BTBusAddress addr;

    status = btAccessor->StartConnectable(addr.addr, addr.psm);
    bool tcSuccess = (status == ER_OK);
    if (tcSuccess) {
        self->SetBusAddress(addr);
        nodeDB.AddNode(self);
    } else {
        String detail = "Call to start connectable returned failure code: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(status);
    }

    return tcSuccess;
}

bool TestDriver::TC_StopConnectable()
{
    bool tcSuccess = true;
    btAccessor->StopConnectable();
    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();
    if (l2capEvent) {
        QStatus status = Event::Wait(*l2capEvent, 500);
        if ((status == ER_OK) ||
            (status == ER_TIMEOUT)) {
            ReportTestDetail("L2CAP connect event object is still valid.");
            tcSuccess = false;
        }
    }

    nodeDB.RemoveNode(self);

    return tcSuccess;
}


bool ClientTestDriver::TC_StartDiscovery()
{
    bool tcSuccess = true;
    QStatus status;
    BDAddressSet ignoreAddrs;
    String detail;
    map<BDAddress, FoundInfo>::iterator fit;

    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        ignoreAddrs->insert(*it);
    }

    if (!opts.fastDiscovery) {
        uint64_t now;
        uint64_t stop;
        Timespec tsNow;

        GetTimeNow(&tsNow);
        now = tsNow.GetAbsoluteMillis();
        stop = now + 35000;

        devChangeLock.Lock();
        devChangeQueue.clear();
        devChangeEvent.ResetEvent();
        devChangeLock.Unlock();

        status = btAccessor->StartDiscovery(ignoreAddrs, 30);
        if (status != ER_OK) {
            detail = "Call to start discovery failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        while (now < stop) {
            status = Event::Wait(devChangeEvent, stop - now);
            if (status == ER_TIMEOUT) {
                break;
            } else if (status != ER_OK) {
                detail = "Wait for device change event failed: ";
                detail += QCC_StatusText(status);
                detail += ".";
                ReportTestDetail(detail);
                tcSuccess = false;
                goto exit;
            }

            devChangeEvent.ResetEvent();

            devChangeLock.Lock();
            while (!devChangeQueue.empty()) {
                fit = foundInfo.find(devChangeQueue.front().addr);
                if (fit == foundInfo.end()) {
                    foundInfo[devChangeQueue.front().addr] = FoundInfo(devChangeQueue.front().uuidRev);
                } else {
                    ++(fit->second.found);
                    if (fit->second.uuidRev != devChangeQueue.front().uuidRev) {
                        ++(fit->second.changed);
                        fit->second.uuidRev = devChangeQueue.front().uuidRev;
                    }
                }
                devChangeQueue.pop_front();
            }
            devChangeLock.Unlock();

            GetTimeNow(&tsNow);
            now = tsNow.GetAbsoluteMillis();
        }

        if (foundInfo.empty()) {
            ReportTestDetail("No devices found");
        } else {
            for (fit = foundInfo.begin(); fit != foundInfo.end(); ++fit) {
                detail = "Found ";
                detail += fit->first.ToString().c_str();
                detail += " ";
                detail += U32ToString(fit->second.found).c_str();
                detail += " times";
                if (fit->second.changed > 0) {
                    detail += " - changed ";
                    detail += U32ToString(fit->second.changed).c_str();
                    detail += " times";
                }
                detail += " (UUID Rev: 0x";
                detail += U32ToString(fit->second.uuidRev, 16, 8, '0');
                detail += ")";
                detail += ".";
                ReportTestDetail(detail);
            }
        }

        Sleep(5000);

        devChangeLock.Lock();
        devChangeQueue.clear();
        devChangeEvent.ResetEvent();
        devChangeLock.Unlock();

        status = Event::Wait(devChangeEvent, 30000);
        if (status != ER_TIMEOUT) {
            ReportTestDetail("Received device found notification long after discovery should have stopped.");
            //tcSuccess = false;
            devChangeLock.Lock();
            devChangeQueue.clear();
            devChangeEvent.ResetEvent();
            devChangeLock.Unlock();
            goto exit;
        }
    }

    // Start infinite discovery until stopped
    status = btAccessor->StartDiscovery(ignoreAddrs, 0);
    if (status != ER_OK) {
        detail = "Call to start discovery with infinite timeout failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_StopDiscovery()
{
    QStatus status;
    bool tcSuccess = true;
    size_t count;
    BDAddressSet ignoreAddrs;
    String detail;

    status = btAccessor->StopDiscovery();
    if (status != ER_OK) {
        detail = "Call to stop discovery failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    if (!opts.fastDiscovery) {
        Sleep(5000);

        devChangeLock.Lock();
        count = devChangeQueue.size();
        devChangeQueue.clear();
        devChangeEvent.ResetEvent();
        devChangeLock.Unlock();

        status = Event::Wait(devChangeEvent, 30000);
        if (status != ER_TIMEOUT) {
            ReportTestDetail("Received device found notification long after discovery should have stopped.");
            //tcSuccess = false;
            devChangeLock.Lock();
            devChangeQueue.clear();
            devChangeEvent.ResetEvent();
            devChangeLock.Unlock();
        }
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_GetDeviceInfo()
{
    QStatus status;
    bool tcSuccess = true;
    map<BDAddress, FoundInfo>::iterator fit;
    bool found = false;

    while (!found) {
        for (fit = foundInfo.begin(); !found && (fit != foundInfo.end()); ++fit) {
            if (!fit->second.checked) {
                status = btAccessor->GetDeviceInfo(fit->first, &connUUIDRev, &connAddr, &connAdInfo);
                if (status != ER_OK) {
                    String detail = "Failed to get device information from ";
                    detail += fit->first.ToString();
                    detail += " (non-critical): ";
                    detail += QCC_StatusText(status);
                    detail += ".";
                    ReportTestDetail(detail);
                } else if (connUUIDRev != bt::INVALID_UUIDREV) {
                    BTNodeDB::const_iterator nit;
                    for (nit = connAdInfo.Begin(); !found && (nit != connAdInfo.End()); ++nit) {
                        NameSet::const_iterator nsit;
                        for (nsit = (*nit)->GetAdvertiseNamesBegin();
                             !found && (nsit != (*nit)->GetAdvertiseNamesEnd());
                             ++nsit) {
                            if (nsit->compare(0, opts.basename.size(), opts.basename) == 0) {
                                found = true;
                            }
                        }
                    }
                }
            }
        }

        if (!found) {
            status = Event::Wait(devChangeEvent, 60000);
            if (status != ER_OK) {
                String detail = "Wait for device change event failed: ";
                detail += QCC_StatusText(status);
                detail += ".";
                ReportTestDetail(detail);
                tcSuccess = false;
                goto exit;
            }

            devChangeEvent.ResetEvent();

            devChangeLock.Lock();
            while (!devChangeQueue.empty()) {
                fit = foundInfo.find(devChangeQueue.front().addr);
                if (fit == foundInfo.end()) {
                    foundInfo[devChangeQueue.front().addr] = FoundInfo(devChangeQueue.front().uuidRev);
                } else {
                    ++(fit->second.found);
                    if (fit->second.uuidRev != devChangeQueue.front().uuidRev) {
                        ++(fit->second.changed);
                        fit->second.uuidRev = devChangeQueue.front().uuidRev;
                    }
                }
                devChangeQueue.pop_front();
            }
            devChangeLock.Unlock();
        }
    }

    if (found) {
        String detail = "Found \"";
        detail += opts.basename;
        detail += "\" in advertisement for device with connect address ";
        detail += connAddr.ToString();
        detail += ".";
        ReportTestDetail(detail);
        connNode = connAdInfo.FindNode(connAddr);
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_ConnectSingle()
{
    bool tcSuccess = true;

    if (!connNode->IsValid()) {
        ReportTestDetail("Cannot continue with connection testing.  Connection address not set (no device found).");
        tcSuccess = false;
        goto exit;
    }

    ep = btAccessor->Connect(bus, connNode);

    if (!ep) {
        String detail = "Failed to connect to ";
        detail += connNode->GetBusAddress().ToString();
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_ConnectMultiple()
{
    bool tcSuccess = true;
    RemoteEndpoint* eps[100];

    memset(eps, 0, sizeof(eps));

    if (!connNode->IsValid()) {
        ReportTestDetail("Cannot continue with connection testing.  Connection address not set (no device found).");
        tcSuccess = false;
        goto exit;
    }

    for (size_t i = 0; i < ArraySize(eps); ++i) {
        eps[i] = btAccessor->Connect(bus, connNode);

        if (!eps[i]) {
            String detail = "Failed connect ";
            detail += U32ToString(i);
            detail += " to ";
            detail += connNode->GetBusAddress().ToString();
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }
    }

exit:
    for (size_t i = 0; i < ArraySize(eps); ++i) {
        if (eps[i]) {
            delete eps[i];
        }
    }

    return tcSuccess;
}

bool ClientTestDriver::TC_ExchangeSmallData()
{
    return ExchangeData(1);
}

bool ClientTestDriver::TC_ExchangeLargeData()
{
    return ExchangeData(256 * 1024);
}

bool ClientTestDriver::TC_IsMaster()
{
    QStatus status;
    bool tcSuccess = true;
    bool master;
    String detail;

    status = btAccessor->IsMaster(connAddr.addr, master);
    if (status != ER_OK) {
        detail = "Failed to get BT master/slave role: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    detail = "We are ";
    detail += master ? "the master." : "a slave.";
    ReportTestDetail(detail);

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_RequestBTRole()
{
    QStatus status;
    bool tcSuccess = true;
    bool oldMaster;
    bool newMaster;
    String detail;

    status = btAccessor->IsMaster(connAddr.addr, oldMaster);
    if (status != ER_OK) {
        detail = "Failed to get BT master/slave role: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    btAccessor->RequestBTRole(connAddr.addr, oldMaster ? bt::SLAVE : bt::MASTER);

    status = btAccessor->IsMaster(connAddr.addr, newMaster);
    if (status != ER_OK) {
        detail = "Failed to get BT master/slave role: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    if (newMaster == oldMaster) {
        ReportTestDetail("Failed to change BT master/slave role.");
        tcSuccess = false;
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::ExchangeData(size_t size)
{
    bool tcSuccess = true;
    size_t bufSize = size * qcc::GUID::SIZE;
    uint8_t* txBuf = new uint8_t[bufSize];
    uint8_t* rxBuf = new uint8_t[bufSize];
    uint8_t* buf = new uint8_t[bufSize];
    uint8_t* expBuf = new uint8_t[bufSize];
    
    for (size_t i = 0; i < bufSize; i += qcc::GUID::SIZE) {
        memcpy(txBuf + i, busGuid.GetBytes(), qcc::GUID::SIZE);
        memcpy(expBuf + i, connNode->GetGUID().GetBytes(), qcc::GUID::SIZE);
    }

    tcSuccess = SendBuf(txBuf, bufSize);
    if (!tcSuccess) {
        goto exit;
    }

    tcSuccess = RecvBuf(rxBuf, bufSize);
    if (!tcSuccess) {
        goto exit;
    }

    XorByteArray(txBuf, rxBuf, buf, bufSize);

    if (memcmp(buf, expBuf, bufSize) != 0) {
        String detail = "Recieved bytes does not match expected.";
        ReportTestDetail(detail);
        tcSuccess = false;
    }

exit:
    delete[] txBuf;
    delete[] rxBuf;
    delete[] buf;
    delete[] expBuf;

    return tcSuccess;
}


bool ServerTestDriver::TC_StartDiscoverability()
{
    bool tcSuccess = true;
    QStatus status;

    status = btAccessor->StartDiscoverability();
    if (status != ER_OK) {
        String detail = "Call to start discoverability failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
    }

    return tcSuccess;
}

bool ServerTestDriver::TC_StopDiscoverability()
{
    bool tcSuccess = true;
    QStatus status;

    status = btAccessor->StopDiscoverability();
    if (status != ER_OK) {
        String detail = "Call to stop discoverability failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
    }

    return tcSuccess;
}

bool ServerTestDriver::TC_SetSDPInfo()
{
    bool tcSuccess = true;
    QStatus status;
    String adName = opts.basename + "." + self->GetBusAddress().addr.ToString('_') + ".";
    String detail;
    int i;

    // Advertise 100 names for the local device.
    for (i = 0; i < 100; ++i) {
        self->AddAdvertiseName(adName + RandHexString(4));
    }

    // Advertise names for 100 nodes
    for (i = 0; i < 100; ++i) {
        BDAddress addr(RandHexString(6));
        BTBusAddress busAddr(addr, Rand32() % 0xffff);
        BTNodeInfo fakeNode;
        int j;
        fakeNode = BTNodeInfo(busAddr);
        adName = opts.basename + "." + fakeNode->GetBusAddress().addr.ToString('_') + ".";
        for (j = 0; j < 5; ++j) {
            fakeNode->AddAdvertiseName(adName + RandHexString(4));
        }
        nodeDB.AddNode(fakeNode);
    }

    status = btAccessor->SetSDPInfo(uuidRev,
                                    self->GetBusAddress().addr,
                                    self->GetBusAddress().psm,
                                    nodeDB);
    if (status != ER_OK) {
        detail = "Call to set SDP information returned failure code: ";
        detail += QCC_StatusText(status);
        tcSuccess = false;
    } else {
        detail = "UUID revision for SDP record set to 0x";
        detail += U32ToString(uuidRev, 16, 8, '0');
    }
    detail += ".";
    ReportTestDetail(detail);

    return tcSuccess;
}

bool ServerTestDriver::TC_GetL2CAPConnectEvent()
{
    bool tcSuccess = false;
    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();
    if (l2capEvent) {
        QStatus status = Event::Wait(*l2capEvent, 500);
        if ((status == ER_OK) ||
            (status == ER_TIMEOUT)) {
            tcSuccess = true;
        } else {
            ReportTestDetail("L2CAP connect event object is invalid.");
        }
    } else {
        ReportTestDetail("L2CAP connect event object does not exist.");
    }
    return tcSuccess;
}

bool ServerTestDriver::TC_AcceptSingle()
{
    bool tcSuccess = true;
    QStatus status;

    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();

    if (!l2capEvent) {
        ReportTestDetail("L2CAP connect event object does not exist.");
        tcSuccess = false;
        goto exit;
    }

    status = Event::Wait(*l2capEvent, 120000);
    if (status != ER_OK) {
        String detail = "Failed to wait for incoming connection: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    ep = btAccessor->Accept(bus, l2capEvent);

    if (!ep) {
        ReportTestDetail("Failed to accept incoming connection.");
        tcSuccess = false;
    }

exit:
    return tcSuccess;
}

bool ServerTestDriver::TC_AcceptMultiple()
{
    bool tcSuccess = true;
    QStatus status;
    RemoteEndpoint* eps[100];

    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();

    memset(eps, 0, sizeof(eps));

    if (!l2capEvent) {
        ReportTestDetail("L2CAP connect event object does not exist.");
        tcSuccess = false;
        goto exit;
    }

    for (size_t i = 0; i < ArraySize(eps); ++i) {
        status = Event::Wait(*l2capEvent, 120000);
        if (status != ER_OK) {
            String detail = "Failed to wait for incoming connection: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        eps[i] = btAccessor->Accept(bus, l2capEvent);

        if (!eps[i]) {
            String detail = "Failed to accept incoming connection ";
            detail += U32ToString(i);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }
    }

exit:
    for (size_t i = 0; i < ArraySize(eps); ++i) {
        if (eps[i]) {
            delete eps[i];
        }
    }

    return tcSuccess;
}

bool ServerTestDriver::TC_ExchangeSmallData()
{
    return ExchangeData(1);
}

bool ServerTestDriver::TC_ExchangeLargeData()
{
    return ExchangeData(256 * 1024);
}

bool ServerTestDriver::ExchangeData(size_t size)
{
    bool tcSuccess = true;
    size_t bufSize = size * qcc::GUID::SIZE;
    uint8_t* txBuf = new uint8_t[bufSize];
    uint8_t* rxBuf = new uint8_t[bufSize];
    uint8_t* buf = new uint8_t[bufSize];

    for (size_t i = 0; i < bufSize; i += qcc::GUID::SIZE) {
        memcpy(buf + i, busGuid.GetBytes(), qcc::GUID::SIZE);
    }

    tcSuccess = RecvBuf(rxBuf, bufSize);
    if (!tcSuccess) {
        goto exit;
    }

    XorByteArray(rxBuf, buf, txBuf, bufSize);

    tcSuccess = SendBuf(txBuf, bufSize);

exit:
    delete[] txBuf;
    delete[] rxBuf;
    delete[] buf;

    return tcSuccess;
}


/****************************************/

ClientTestDriver::ClientTestDriver(const CmdLineOptions& opts) :
    TestDriver(opts)
{
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_StartDiscovery), "Start Discovery (~70 sec)");
    if (!opts.local) {
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_GetDeviceInfo), "Get Device Information");
    }
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_StopDiscovery), "Stop Discovery (~35 sec)");
    if (!opts.local) {
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_ConnectSingle), "Single Connection to Server");
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_ConnectMultiple), "Multiple Simultaneous Connections to Server");
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_ExchangeSmallData), "Exchange Small Amount of Data");
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_ExchangeLargeData), "Exchange Large Amount of Data");
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_IsMaster), "Check BT master/slave role");
        AddTestCase(TestCaseFunction(ClientTestDriver::TC_RequestBTRole), "Switch BT master/slave role");
    }
}


ServerTestDriver::ServerTestDriver(const CmdLineOptions& opts) :
    TestDriver(opts),
    allowIncomingAddress(true)
{
    while (uuidRev == bt::INVALID_UUIDREV) {
        uuidRev = Rand32();
    }

    AddTestCase(TestCaseFunction(ServerTestDriver::TC_SetSDPInfo), "Set SDP Information");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_GetL2CAPConnectEvent), "Check L2CAP Connect Event Object");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_StartDiscoverability), "Start Discoverability");
    if (!opts.local) {
        AddTestCase(TestCaseFunction(ServerTestDriver::TC_AcceptSingle), "Accept Single Incoming Connection");
        AddTestCase(TestCaseFunction(ServerTestDriver::TC_AcceptMultiple), "Accept Multiple Incoming Connections");
        AddTestCase(TestCaseFunction(ServerTestDriver::TC_ExchangeSmallData), "Exchange Small Amount of Data");
        AddTestCase(TestCaseFunction(ServerTestDriver::TC_ExchangeLargeData), "Exchange Large Amount of Data");
    }
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_StopDiscoverability), "Stop Discoverability");
}


/****************************************/

static void Usage(void)
{
    printf("Usage: BTAccessorTester [-h] [-c | -s] [-n <basename>] [-a] [-d]\n"
           "\n"
           "    -h              Print this help message\n"
           "    -c              Run in client mode\n"
           "    -s              Run in server mode\n"
           "    -n <basename>   Set the base name for advertised/find names\n"
           "    -a              Automatic tests only (disable interactive tests)\n"
           "    -l              Only run local tests (skip inter-device tests)\n"
           "    -f              Fast discovery (client only - skips some discovery testing)\n"
           "    -q              Quiet - suppress debug and log errors\n"
           "    -d              Output test details\n");
}

static void ParseCmdLine(int argc, char** argv, CmdLineOptions& opts)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            Usage();
            exit(0);
        } else if (strcmp(argv[i], "-c") == 0) {
            if (opts.server) {
                printf("Cannot specify server and client at the same time.\n");
                Usage();
                exit(-1);
            }
            opts.client = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (opts.client) {
                printf("Cannot specify server and client at the same time.\n");
                Usage();
                exit(-1);
            }
            opts.server = true;
        } else if (strcmp(argv[i], "-n") == 0) {
            ++i;
            if (i == argc) {
                printf("option %s requires a parameter\n", argv[i - 1]);
                Usage();
                exit(-1);
            } else {
                opts.basename = argv[i];
            }
        } else if (strcmp(argv[i], "-a") == 0) {
            opts.allowInteractive = false;
        } else if (strcmp(argv[i], "-d") == 0) {
            opts.reportDetails = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            opts.local = true;
        } else if (strcmp(argv[i], "-f") == 0) {
            opts.fastDiscovery = true;
        } else if (strcmp(argv[i], "-q") == 0) {
            opts.quiet = true;
        }
    }
}

void DebugOutputHandler(DbgMsgType type,
                        const char* module,
                        const char* msg,
                        void* context)
{
}

int main(int argc, char** argv)
{
#if defined(NDEBUG) && defined(QCC_OS_ANDROID)
    LoggerSetting::GetLoggerSetting("bbdaemon", LOG_ERR, true, NULL);
#else
    LoggerSetting::GetLoggerSetting("bbdaemon", LOG_DEBUG, false, stdout);
#endif

    TestDriver* driver;

    CmdLineOptions opts;

    ParseCmdLine(argc, argv, opts);

    if (opts.quiet) {
        QCC_RegisterOutputCallback(DebugOutputHandler, NULL);
    }

    if (opts.client) {
        driver = new ClientTestDriver(opts);
    } else if (opts.server) {
        driver = new ServerTestDriver(opts);
    } else {
        driver = new TestDriver(opts);
    }

    int ret = driver->RunTests();
    delete driver;

    return ret;
}
