/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "track.hpp"
#include "dtlssrtptransport.hpp"
#include "include.hpp"

namespace rtc {

using std::shared_ptr;
using std::weak_ptr;

Track::Track(Description::Media description)
    : mMediaDescription(std::move(description)), mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {}

string Track::mid() const { return mMediaDescription.mid(); }

Description::Media Track::description() const { return mMediaDescription; }

void Track::close() {
	mIsClosed = true;
	resetCallbacks();
	setRtcpHandler(nullptr);
}

bool Track::send(message_variant data) { return outgoing(make_message(std::move(data))); }

bool Track::send(const byte *data, size_t size) {
	return outgoing(std::make_shared<Message>(data, data + size, Message::Binary));
}

std::optional<message_variant> Track::receive() {
	if (!mRecvQueue.empty())
		return to_variant(std::move(**mRecvQueue.pop()));

	return nullopt;
}

bool Track::isOpen(void) const {
#if RTC_ENABLE_MEDIA
	return !mIsClosed && mDtlsSrtpTransport.lock();
#else
	return !mIsClosed;
#endif
}

bool Track::isClosed(void) const { return mIsClosed; }

size_t Track::maxMessageSize() const {
	return 65535 - 12 - 4; // SRTP/UDP
}

size_t Track::availableAmount() const {
	return mRecvQueue.amount();
}

#if RTC_ENABLE_MEDIA
void Track::open(shared_ptr<DtlsSrtpTransport> transport) {
	mDtlsSrtpTransport = transport;
	triggerOpen();
}
#endif

bool Track::outgoing(message_ptr message) {
	auto direction = mMediaDescription.direction();
	if (direction == Description::Direction::RecvOnly ||
	    direction == Description::Direction::Inactive)
		throw std::runtime_error("Track media direction does not allow sending");

	if (mIsClosed)
		throw std::runtime_error("Track is closed");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

#if RTC_ENABLE_MEDIA
	auto transport = mDtlsSrtpTransport.lock();
	if (!transport)
		throw std::runtime_error("Track transport is not open");

	return transport->sendMedia(message);
#else
	PLOG_WARNING << "Ignoring track send (not compiled with SRTP support)";
	return false;
#endif
}

void Track::incoming(message_ptr message) {
	if (!message)
		return;

	if (mRtcpHandler) {
		auto opt = mRtcpHandler->incoming(message);
		if (!opt)
			return;

		message = *opt;
	}

	auto direction = mMediaDescription.direction();
	if ((direction == Description::Direction::SendOnly ||
	     direction == Description::Direction::Inactive) &&
	    message->type != Message::Control) {
		PLOG_WARNING << "Track media direction does not allow reception, dropping";
	}

	// Tail drop if queue is full
	if (mRecvQueue.full())
		return;

	mRecvQueue.push(message);
	triggerAvailable(mRecvQueue.size());
}

void Track::setRtcpHandler(std::shared_ptr<RtcpHandler> handler) {
	if (mRtcpHandler)
		mRtcpHandler->onOutgoing(nullptr);

	mRtcpHandler = std::move(handler);
	if (mRtcpHandler) {
		mRtcpHandler->onOutgoing([&]([[maybe_unused]] message_ptr message) {
#if RTC_ENABLE_MEDIA
			if (auto transport = mDtlsSrtpTransport.lock())
				transport->sendMedia(message);
#else
			PLOG_WARNING << "Ignoring RTCP send (not compiled with SRTP support)";
#endif
		});
	}
}

} // namespace rtc

