/******************************************************************************
 *
 * Copyright 2011-2012, Qualcomm Innovation Center, Inc.
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
 *
 *****************************************************************************/

#pragma once

#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/MessageReceiver.h>
#include <alljoyn/Message.h>
#include <qcc/String.h>
#include <Message.h>
#include <InterfaceDescription.h>
#include <InterfaceMember.h>
#include <qcc/winrt/utility.h>
#include <qcc/ManagedObj.h>
#include <qcc/Mutex.h>
#include <map>
#include <ObjectReference.h>
#include <AllJoynException.h>
#include <BusAttachment.h>

namespace AllJoyn {

ref class InterfaceMember;
ref class Message;
ref class MessageReceiver;
ref class BusAttachment;
ref class BusObject;
ref class ProxyBusObject;

public delegate void MessageReceiverMethodHandler(InterfaceMember ^ member, Message ^ message);
public delegate void MessageReceiverReplyHandler(Message ^ message, Platform::Object ^ context);
public delegate void MessageReceiverSignalHandler(InterfaceMember ^ member, Platform::String ^ srcPath, Message ^ message);

ref class __MessageReceiver {
  private:
    friend ref class MessageReceiver;
    friend class _MessageReceiver;
    __MessageReceiver() { }
    ~__MessageReceiver() { }

    event MessageReceiverMethodHandler ^ MethodHandler;
    event MessageReceiverReplyHandler ^ ReplyHandler;
    event MessageReceiverSignalHandler ^ SignalHandler;
};

class _MessageReceiver : protected ajn::MessageReceiver {
  protected:
    friend class qcc::ManagedObj<_MessageReceiver>;
    friend ref class MessageReceiver;
    friend ref class BusAttachment;
    friend ref class BusObject;
    friend ref class ProxyBusObject;
    friend class _BusObject;
    friend class _ProxyBusObject;
    _MessageReceiver(BusAttachment ^ bus)
        : Bus(bus)
    {
        ::QStatus status = ER_OK;

        while (true) {
            _eventsAndProperties = ref new __MessageReceiver();
            if (nullptr == _eventsAndProperties) {
                status = ER_OUT_OF_MEMORY;
                break;
            }
            _eventsAndProperties->MethodHandler += ref new MessageReceiverMethodHandler([&] (InterfaceMember ^ member, Message ^ message) {
                                                                                            DefaultMessageReceiverMethodHandler(member, message);
                                                                                        });
            _eventsAndProperties->ReplyHandler += ref new MessageReceiverReplyHandler([&] (Message ^ message, Platform::Object ^ context) {
                                                                                          DefaultMessageReceiverReplyHandler(message, context);
                                                                                      });
            _eventsAndProperties->SignalHandler += ref new MessageReceiverSignalHandler([&] (InterfaceMember ^ member, Platform::String ^ srcPath, Message ^ message) {
                                                                                            DefaultMessageReceiverSignalHandler(member, srcPath, message);
                                                                                        });
            break;
        }

        if (ER_OK != status) {
            QCC_THROW_EXCEPTION(status);
        }
    }

    ~_MessageReceiver()
    {
        _eventsAndProperties = nullptr;
        Bus = nullptr;
    }

    void DefaultMessageReceiverMethodHandler(InterfaceMember ^ member, Message ^ message)
    {
    }

    void DefaultMessageReceiverReplyHandler(Message ^ message, Platform::Object ^ context)
    {
    }

    void DefaultMessageReceiverSignalHandler(InterfaceMember ^ member, Platform::String ^ srcPath, Message ^ message)
    {
    }

    ajn::MessageReceiver::MethodHandler GetMethodHandler()
    {
        return static_cast<ajn::MessageReceiver::MethodHandler>(&AllJoyn::_MessageReceiver::MethodHandler);
    }

    ajn::MessageReceiver::ReplyHandler GetReplyHandler()
    {
        return static_cast<ajn::MessageReceiver::ReplyHandler>(&AllJoyn::_MessageReceiver::ReplyHandler);
    }

    ajn::MessageReceiver::SignalHandler GetSignalHandler()
    {
        return static_cast<ajn::MessageReceiver::SignalHandler>(&AllJoyn::_MessageReceiver::SignalHandler);
    }

    void MethodHandler(const ajn::InterfaceDescription::Member* member, ajn::Message& msg)
    {
        ::QStatus status = ER_OK;

        try {
            while (true) {
                InterfaceMember ^ imember = ref new InterfaceMember((void*)member);
                if (nullptr == imember) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Message ^ message = ref new Message((void*)&msg, true);
                if (nullptr == message) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Bus->_busAttachment->DispatchCallback(ref new Windows::UI::Core::DispatchedHandler([&]() {
                                                                                                       _eventsAndProperties->MethodHandler(imember, message);

                                                                                                   }));
                break;
            }

            if (ER_OK != status) {
                QCC_THROW_EXCEPTION(status);
            }
        } catch (...) {
            // Do nothing
        }
    }

    void ReplyHandler(ajn::Message& msg, void* context)
    {
        ::QStatus status = ER_OK;

        try {
            while (true) {
                Message ^ message = ref new Message((void*)&msg, true);
                if (nullptr == message) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Platform::Object ^ objContext = reinterpret_cast<Platform::Object ^>(context);
                Bus->_busAttachment->DispatchCallback(ref new Windows::UI::Core::DispatchedHandler([&]() {
                                                                                                       _eventsAndProperties->ReplyHandler(message, objContext);
                                                                                                   }));
                break;
            }

            if (ER_OK != status) {
                QCC_THROW_EXCEPTION(status);
            }
        } catch (...) {
            // Do nothing
        }
    }

    void SignalHandler(const ajn::InterfaceDescription::Member* member, const char* srcPath, ajn::Message& msg)
    {
        ::QStatus status = ER_OK;

        try {
            while (true) {
                InterfaceMember ^ imember = ref new InterfaceMember((void*)member);
                if (nullptr == imember) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Platform::String ^ strSrcPath = MultibyteToPlatformString(srcPath);
                if (nullptr == strSrcPath && srcPath != NULL && srcPath[0] != '\0') {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Message ^ message = ref new Message((void*)&msg, true);
                if (nullptr == message) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                Bus->_busAttachment->DispatchCallback(ref new Windows::UI::Core::DispatchedHandler([&]() {
                                                                                                       _eventsAndProperties->SignalHandler(imember, strSrcPath, message);
                                                                                                   }));
                break;
            }

            if (ER_OK != status) {
                QCC_THROW_EXCEPTION(status);
            }
        } catch (...) {
            // Do nothing
        }
    }

    __MessageReceiver ^ _eventsAndProperties;
    BusAttachment ^ Bus;
};

public ref class MessageReceiver sealed {
  public:
    MessageReceiver(BusAttachment ^ bus)
    {
        ::QStatus status = ER_OK;

        while (true) {
            if (nullptr == bus) {
                status = ER_BAD_ARG_1;
                break;
            }
            _MessageReceiver* mr = new _MessageReceiver(bus);
            if (NULL == mr) {
                status = ER_OUT_OF_MEMORY;
                break;
            }
            _mReceiver = new qcc::ManagedObj<_MessageReceiver>(mr);
            if (NULL == _mReceiver) {
                status = ER_OUT_OF_MEMORY;
                break;
            }
            _receiver = &(**_mReceiver);
            break;
        }

        if (ER_OK != status) {
            QCC_THROW_EXCEPTION(status);
        }
    }

    event MessageReceiverMethodHandler ^ MethodHandler
    {
        Windows::Foundation::EventRegistrationToken add(MessageReceiverMethodHandler ^ handler)
        {
            return _receiver->_eventsAndProperties->MethodHandler::add(handler);
        }

        void remove(Windows::Foundation::EventRegistrationToken token)
        {
            _receiver->_eventsAndProperties->MethodHandler::remove(token);
        }

        void raise(InterfaceMember ^ member, Message ^ message)
        {
            _receiver->_eventsAndProperties->MethodHandler::raise(member, message);
        }
    }

    event MessageReceiverReplyHandler ^ ReplyHandler
    {
        Windows::Foundation::EventRegistrationToken add(MessageReceiverReplyHandler ^ handler)
        {
            return _receiver->_eventsAndProperties->ReplyHandler::add(handler);
        }

        void remove(Windows::Foundation::EventRegistrationToken token)
        {
            _receiver->_eventsAndProperties->ReplyHandler::remove(token);
        }

        void raise(Message ^ message, Platform::Object ^ context)
        {
            _receiver->_eventsAndProperties->ReplyHandler::raise(message, context);
        }
    }

    event MessageReceiverSignalHandler ^ SignalHandler
    {
        Windows::Foundation::EventRegistrationToken add(MessageReceiverSignalHandler ^ handler)
        {
            return _receiver->_eventsAndProperties->SignalHandler::add(handler);
        }

        void remove(Windows::Foundation::EventRegistrationToken token)
        {
            _receiver->_eventsAndProperties->SignalHandler::remove(token);
        }

        void raise(InterfaceMember ^ member, Platform::String ^ srcPath, Message ^ message)
        {
            _receiver->_eventsAndProperties->SignalHandler::raise(member, srcPath, message);
        }
    }

  private:
    friend ref class ProxyBusObject;
    friend ref class BusObject;
    friend ref class BusAttachment;
    friend class _BusObject;
    friend class _ProxyBusObject;
    MessageReceiver()
    {
    }

    MessageReceiver(void* receiver, bool isManaged)
    {
        ::QStatus status = ER_OK;

        while (true) {
            if (NULL == receiver) {
                status = ER_BAD_ARG_1;
                break;
            }
            if (!isManaged) {
                status = ER_FAIL;
                break;
            }           else {
                qcc::ManagedObj<_MessageReceiver>* mmr = reinterpret_cast<qcc::ManagedObj<_MessageReceiver>*>(receiver);
                _mReceiver = new qcc::ManagedObj<_MessageReceiver>(*mmr);
                if (NULL == _mReceiver) {
                    status = ER_OUT_OF_MEMORY;
                    break;
                }
                _receiver = &(**_mReceiver);
            }
            break;
        }

        if (ER_OK != status) {
            QCC_THROW_EXCEPTION(status);
        }
    }

    ~MessageReceiver()
    {
        if (NULL != _mReceiver) {
            delete _mReceiver;
            _mReceiver = NULL;
            _receiver = NULL;
        }
    }

    qcc::ManagedObj<_MessageReceiver>* _mReceiver;
    _MessageReceiver* _receiver;
};

}
// MessageReceiver.h
