// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REMOTING_PROTOCOL_HOST_CONTROL_DISPATCHER_H_
#define REMOTING_PROTOCOL_HOST_CONTROL_DISPATCHER_H_

#include "base/memory/ref_counted.h"
#include "remoting/protocol/channel_dispatcher_base.h"
#include "remoting/protocol/client_stub.h"
#include "remoting/protocol/clipboard_stub.h"
#include "remoting/protocol/message_reader.h"

namespace net {
class StreamSocket;
}  // namespace net

namespace remoting {
namespace protocol {

class BufferedSocketWriter;
class ControlMessage;
class HostStub;
class Session;

// HostControlDispatcher dispatches incoming messages on the control
// channel to HostStub or ClipboardStub, and also implements ClientStub and
// ClipboardStub for outgoing messages.
class HostControlDispatcher : public ChannelDispatcherBase, public ClientStub,
                              public ClipboardStub {
 public:
  HostControlDispatcher();
  virtual ~HostControlDispatcher();

  // ClipboardStub implementation.
  virtual void InjectClipboardEvent(const ClipboardEvent& event) OVERRIDE;

  // Sets the ClipboardStub that will be called for each incoming clipboard
  // message. |clipboard_stub| must outlive this object.
  void set_clipboard_stub(ClipboardStub* clipboard_stub) {
    clipboard_stub_ = clipboard_stub;
  }

  // Sets the HostStub that will be called for each incoming control
  // message. |host_stub| must outlive this object.
  void set_host_stub(HostStub* host_stub) { host_stub_ = host_stub; }

 protected:
  // ChannelDispatcherBase overrides.
  virtual void OnInitialized() OVERRIDE;

 private:
  void OnMessageReceived(scoped_ptr<ControlMessage> message,
                         const base::Closure& done_task);

  ClipboardStub* clipboard_stub_;
  HostStub* host_stub_;

  ProtobufMessageReader<ControlMessage> reader_;
  scoped_refptr<BufferedSocketWriter> writer_;

  DISALLOW_COPY_AND_ASSIGN(HostControlDispatcher);
};

}  // namespace protocol
}  // namespace remoting

#endif  // REMOTING_PROTOCOL_HOST_CONTROL_DISPATCHER_H_
