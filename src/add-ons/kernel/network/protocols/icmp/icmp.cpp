/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


/*!	RFC 792 details the ICMP protocol, RFC 1122 lists when an ICMP error must,
	shall, or must not be sent.
*/


#include "icmp.h"

#include <algorithm>
#include <netinet/in.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>

#include <lock.h>
#include <net_datalink.h>
#include <net_protocol.h>
#include <net_stack.h>
#include <NetBufferUtilities.h>
#include <NetUtilities.h>
#include <ProtocolUtilities.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include "ipv4.h"


//#define TRACE_ICMP
#ifdef TRACE_ICMP
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


struct icmp_header {
	uint8	type;
	uint8	code;
	uint16	checksum;
	union {
		struct {
			uint16	id;
			uint16	sequence;
		} echo;
		struct {
			in_addr_t gateway;
		} redirect;
		struct {
			uint16	_reserved;
			uint16	next_mtu;
		} path_mtu;
		struct {
			uint8	pointer;
			uint8	_reserved[3];
		} parameter_problem;

		uint32 zero;
	};
};

typedef NetBufferField<uint16, offsetof(icmp_header, checksum)>
	ICMPChecksumField;


// A datagram ICMP socket is a "ping socket": anyone may open one, but it can
// only send echo requests, carrying the socket's own identifier, and it only
// receives the replies to them.
class PingSocket
	: public DoublyLinkedListLinkImpl<PingSocket>, public DatagramSocket<> {
public:
	PingSocket(net_socket* socket)
		:
		DatagramSocket<>("icmp ping socket", socket),
		identifier(0)
	{
	}

	uint16	identifier;
};

typedef DoublyLinkedList<PingSocket> PingSocketList;


struct icmp_protocol : net_protocol {
	PingSocket*	ping;
};


net_buffer_module_info* gBufferModule;
net_stack_module_info* gStackModule;

static PingSocketList sPingSockets;
static mutex sPingSocketsLock = MUTEX_INITIALIZER("icmp ping sockets");
static uint16 sNextPingIdentifier = 1;


#ifdef TRACE_ICMP


static const char*
net_error_to_string(net_error error)
{
#define CODE(x) case x: return #x;
	switch (error) {
		CODE(B_NET_ERROR_REDIRECT_HOST)
		CODE(B_NET_ERROR_UNREACH_NET)
		CODE(B_NET_ERROR_UNREACH_HOST)
		CODE(B_NET_ERROR_UNREACH_PROTOCOL)
		CODE(B_NET_ERROR_UNREACH_PORT)
		CODE(B_NET_ERROR_MESSAGE_SIZE)
		CODE(B_NET_ERROR_TRANSIT_TIME_EXCEEDED)
		CODE(B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED)
		CODE(B_NET_ERROR_PARAMETER_PROBLEM)
		CODE(B_NET_ERROR_QUENCH)
		default:
			return "unknown";
	}
#undef CODE
}


#endif	// TRACE_ICMP


static net_domain*
get_domain(struct net_buffer* buffer)
{
	net_domain* domain;
	if (buffer->interface_address != NULL)
		domain = buffer->interface_address->domain;
	else
		domain = gStackModule->get_domain(buffer->source->sa_family);

	if (domain == NULL || domain->module == NULL)
		return NULL;

	return domain;
}


static void
fill_sockaddr_in(sockaddr_in* target, in_addr_t address)
{
	target->sin_family = AF_INET;
	target->sin_len = sizeof(sockaddr_in);
	target->sin_port = 0;
	target->sin_addr.s_addr = address;
}


static bool
is_icmp_error(uint8 type)
{
	return type == ICMP_TYPE_UNREACH
		|| type == ICMP_TYPE_PARAMETER_PROBLEM
		|| type == ICMP_TYPE_REDIRECT
		|| type == ICMP_TYPE_TIME_EXCEEDED
		|| type == ICMP_TYPE_SOURCE_QUENCH;
}


static net_error
icmp_to_net_error(uint8 type, uint8 code)
{
	switch (type) {
		case ICMP_TYPE_UNREACH:
			switch (code) {
				case ICMP_CODE_UNREACH_NET:
					return B_NET_ERROR_UNREACH_NET;
				case ICMP_CODE_UNREACH_HOST:
					return B_NET_ERROR_UNREACH_HOST;
				case ICMP_CODE_UNREACH_PROTOCOL:
					return B_NET_ERROR_UNREACH_PROTOCOL;
				case ICMP_CODE_UNREACH_PORT:
					return B_NET_ERROR_UNREACH_PORT;
				case ICMP_CODE_UNREACH_FRAGMENTATION_NEEDED:
					return B_NET_ERROR_MESSAGE_SIZE;
				case ICMP_CODE_UNREACH_SOURCE_ROUTE_FAIL:
					return B_NET_ERROR_UNREACH_SOURCE_FAIL;
				case ICMP_CODE_UNREACH_NET_UNKNOWN:
					return B_NET_ERROR_UNREACH_NET_UNKNOWN;
				case ICMP_CODE_UNREACH_HOST_UNKNOWN:
					return B_NET_ERROR_UNREACH_HOST_UNKNOWN;
				case ICMP_CODE_UNREACH_ISOLATED:
					return B_NET_ERROR_UNREACH_ISOLATED;
				case ICMP_CODE_UNREACH_NET_PROHIBITED:
					return B_NET_ERROR_UNREACH_NET_PROHIBITED;
				case ICMP_CODE_UNREACH_HOST_PROHIBITED:
					return B_NET_ERROR_UNREACH_HOST_PROHIBITED;
				case ICMP_CODE_UNREACH_NET_TOS:
					return B_NET_ERROR_UNREACH_NET_TOS;
				case ICMP_CODE_UNREACH_HOST_TOS:
					return B_NET_ERROR_UNREACH_HOST_TOS;
				case ICMP_CODE_UNREACH_FILTER_PROHIBITED:
					return B_NET_ERROR_UNREACH_FILTER_PROHIBITED;
				case ICMP_CODE_UNREACH_HOST_PRECEDENCE:
					return B_NET_ERROR_UNREACH_HOST_PRECEDENCE;
				case ICMP_CODE_UNREACH_PRECEDENCE_CUTOFF:
					return B_NET_ERROR_UNREACH_PRECEDENCE_CUTOFF;
			}
			break;

		case ICMP_TYPE_PARAMETER_PROBLEM:
			return B_NET_ERROR_PARAMETER_PROBLEM;

		case ICMP_TYPE_REDIRECT:
			return B_NET_ERROR_REDIRECT_HOST;

		case ICMP_TYPE_SOURCE_QUENCH:
			return B_NET_ERROR_QUENCH;

		case ICMP_TYPE_TIME_EXCEEDED:
			switch (code) {
				case ICMP_CODE_TIME_EXCEEDED_IN_TRANSIT:
					return B_NET_ERROR_TRANSIT_TIME_EXCEEDED;
				case ICMP_CODE_REASSEMBLY_TIME_EXCEEDED:
					return B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED;
			}
			break;

		default:
			break;
	}

	return (net_error)0;
}


static void
net_error_to_icmp(net_error error, uint8& type, uint8& code)
{
	switch (error) {
		// redirect
		case B_NET_ERROR_REDIRECT_HOST:
			type = ICMP_TYPE_REDIRECT;
			code = ICMP_CODE_REDIRECT_HOST;
			break;

		// unreach
		case B_NET_ERROR_UNREACH_NET:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_NET;
			break;
		case B_NET_ERROR_UNREACH_HOST:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_HOST;
			break;
		case B_NET_ERROR_UNREACH_PROTOCOL:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_PROTOCOL;
			break;
		case B_NET_ERROR_UNREACH_PORT:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_PORT;
			break;
		case B_NET_ERROR_MESSAGE_SIZE:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_FRAGMENTATION_NEEDED;
			break;
		case B_NET_ERROR_UNREACH_SOURCE_FAIL:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_SOURCE_ROUTE_FAIL;
			break;
		case B_NET_ERROR_UNREACH_NET_UNKNOWN:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_NET_UNKNOWN;
			break;
		case B_NET_ERROR_UNREACH_HOST_UNKNOWN:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_HOST_UNKNOWN;
			break;
		case B_NET_ERROR_UNREACH_ISOLATED:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_ISOLATED;
			break;
		case B_NET_ERROR_UNREACH_NET_PROHIBITED:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_NET_PROHIBITED;
			break;
		case B_NET_ERROR_UNREACH_HOST_PROHIBITED:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_HOST_PROHIBITED;
			break;
		case B_NET_ERROR_UNREACH_NET_TOS:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_NET_TOS;
			break;
		case B_NET_ERROR_UNREACH_HOST_TOS:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_HOST_TOS;
			break;
		case B_NET_ERROR_UNREACH_FILTER_PROHIBITED:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_FILTER_PROHIBITED;
			break;
		case B_NET_ERROR_UNREACH_HOST_PRECEDENCE:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_HOST_PRECEDENCE;
			break;
		case B_NET_ERROR_UNREACH_PRECEDENCE_CUTOFF:
			type = ICMP_TYPE_UNREACH;
			code = ICMP_CODE_UNREACH_PRECEDENCE_CUTOFF;
			break;

		// time exceeded
		case B_NET_ERROR_TRANSIT_TIME_EXCEEDED:
			type = ICMP_TYPE_TIME_EXCEEDED;
			code = ICMP_CODE_TIME_EXCEEDED_IN_TRANSIT;
			break;
		case B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED:
			type = ICMP_TYPE_TIME_EXCEEDED;
			code = ICMP_CODE_REASSEMBLY_TIME_EXCEEDED;
			break;

		// other
		case B_NET_ERROR_PARAMETER_PROBLEM:
			type = ICMP_TYPE_PARAMETER_PROBLEM;
			code = 0;
			break;
		case B_NET_ERROR_QUENCH:
			type = ICMP_TYPE_SOURCE_QUENCH;
			code = 0;
			break;
	}
}


// #pragma mark - module API


net_protocol*
icmp_init_protocol(net_socket* socket)
{
	icmp_protocol* protocol = new(std::nothrow) icmp_protocol;
	if (protocol == NULL)
		return NULL;

	protocol->ping = NULL;
	return protocol;
}


status_t
icmp_uninit_protocol(net_protocol* protocol)
{
	delete protocol;
	return B_OK;
}


//! Called with sPingSocketsLock held.
static bool
ping_identifier_in_use(uint16 identifier)
{
	PingSocketList::Iterator iterator = sPingSockets.GetIterator();
	while (PingSocket* ping = iterator.Next()) {
		if (ping->identifier == identifier)
			return true;
	}
	return false;
}


status_t
icmp_open(net_protocol* _protocol)
{
	icmp_protocol* protocol = (icmp_protocol*)_protocol;

	PingSocket* ping = new(std::nothrow) PingSocket(protocol->socket);
	if (ping == NULL)
		return B_NO_MEMORY;

	status_t status = ping->InitCheck();
	if (status != B_OK) {
		delete ping;
		return status;
	}

	protocol->ping = ping;
	MutexLocker locker(sPingSocketsLock);
	sPingSockets.Add(ping);
	return B_OK;
}


status_t
icmp_close(net_protocol* _protocol)
{
	icmp_protocol* protocol = (icmp_protocol*)_protocol;
	PingSocket* ping = protocol->ping;
	if (ping == NULL)
		return B_OK;

	MutexLocker locker(sPingSocketsLock);
	sPingSockets.Remove(ping);
	locker.Unlock();

	delete ping;
	protocol->ping = NULL;
	return B_OK;
}


status_t
icmp_free(net_protocol* protocol)
{
	return B_OK;
}


status_t
icmp_connect(net_protocol* protocol, const struct sockaddr* address)
{
	return B_ERROR;
}


status_t
icmp_accept(net_protocol* protocol, struct net_socket** _acceptedSocket)
{
	return B_NOT_SUPPORTED;
}


status_t
icmp_control(net_protocol* protocol, int level, int option, void* value,
	size_t* _length)
{
	return protocol->next->module->control(protocol->next, level, option,
		value, _length);
}


status_t
icmp_getsockopt(net_protocol* protocol, int level, int option, void* value,
	int* length)
{
	return protocol->next->module->getsockopt(protocol->next, level, option,
		value, length);
}


status_t
icmp_setsockopt(net_protocol* protocol, int level, int option,
	const void* value, int length)
{
	return protocol->next->module->setsockopt(protocol->next, level, option,
		value, length);
}


/*!	The port of a ping socket's address is its echo identifier; binding to
	port 0 picks a free one.
*/
status_t
icmp_bind(net_protocol* _protocol, const struct sockaddr* address)
{
	icmp_protocol* protocol = (icmp_protocol*)_protocol;
	PingSocket* ping = protocol->ping;
	if (ping == NULL)
		return B_ERROR;
	if (address->sa_family != AF_INET)
		return EAFNOSUPPORT;

	uint16 identifier = ((const sockaddr_in*)address)->sin_port;

	MutexLocker locker(sPingSocketsLock);
	if (identifier == 0) {
		for (int attempt = 0; attempt < 65535; attempt++) {
			uint16 candidate = htons(sNextPingIdentifier++);
			if (candidate != 0 && !ping_identifier_in_use(candidate)) {
				identifier = candidate;
				break;
			}
		}
		if (identifier == 0)
			return EADDRINUSE;
	} else if (ping_identifier_in_use(identifier))
		return EADDRINUSE;

	ping->identifier = identifier;
	((sockaddr_in*)&protocol->socket->address)->sin_port = identifier;
	return B_OK;
}


status_t
icmp_unbind(net_protocol* _protocol, struct sockaddr* address)
{
	icmp_protocol* protocol = (icmp_protocol*)_protocol;
	if (protocol->ping == NULL)
		return B_ERROR;

	MutexLocker locker(sPingSocketsLock);
	protocol->ping->identifier = 0;
	return B_OK;
}


status_t
icmp_listen(net_protocol* protocol, int count)
{
	return B_NOT_SUPPORTED;
}


status_t
icmp_shutdown(net_protocol* protocol, int direction)
{
	return B_NOT_SUPPORTED;
}


static status_t
prepare_echo_request(icmp_protocol* protocol, net_buffer* buffer)
{
	PingSocket* ping = protocol->ping;
	if (ping == NULL)
		return B_ERROR;

	NetBufferHeaderReader<icmp_header> header(buffer);
	if (header.Status() != B_OK)
		return EINVAL;
	if (header->type != ICMP_TYPE_ECHO_REQUEST || header->code != 0)
		return EINVAL;

	header->echo.id = ping->identifier;
	header->checksum = 0;
	header.Sync();
	*ICMPChecksumField(buffer) = gBufferModule->checksum(buffer, 0,
		buffer->size, true);
	buffer->protocol = IPPROTO_ICMP;
	return B_OK;
}


status_t
icmp_send_data(net_protocol* protocol, net_buffer* buffer)
{
	status_t status = prepare_echo_request((icmp_protocol*)protocol, buffer);
	if (status != B_OK)
		return status;

	return protocol->next->module->send_data(protocol->next, buffer);
}


status_t
icmp_send_routed_data(net_protocol* protocol, struct net_route* route,
	net_buffer* buffer)
{
	status_t status = prepare_echo_request((icmp_protocol*)protocol, buffer);
	if (status != B_OK)
		return status;

	return protocol->next->module->send_routed_data(protocol->next, route,
		buffer);
}


ssize_t
icmp_send_avail(net_protocol* protocol)
{
	return protocol->socket->send.buffer_size;
}


status_t
icmp_read_data(net_protocol* _protocol, size_t numBytes, uint32 flags,
	net_buffer** _buffer)
{
	PingSocket* ping = ((icmp_protocol*)_protocol)->ping;
	if (ping == NULL)
		return B_ERROR;

	return ping->Dequeue(flags, _buffer);
}


ssize_t
icmp_read_avail(net_protocol* _protocol)
{
	PingSocket* ping = ((icmp_protocol*)_protocol)->ping;
	if (ping == NULL)
		return B_ERROR;

	return ping->AvailableData();
}


static void
deliver_echo_reply(net_buffer* buffer, uint16 identifier)
{
	MutexLocker locker(sPingSocketsLock);
	PingSocketList::Iterator iterator = sPingSockets.GetIterator();
	while (PingSocket* ping = iterator.Next()) {
		if (ping->identifier == identifier) {
			ping->EnqueueClone(buffer);
			return;
		}
	}
}


struct net_domain*
icmp_get_domain(net_protocol* protocol)
{
	return protocol->next->module->get_domain(protocol->next);
}


size_t
icmp_get_mtu(net_protocol* protocol, const struct sockaddr* address)
{
	return protocol->next->module->get_mtu(protocol->next, address);
}


status_t
icmp_receive_data(net_buffer* buffer)
{
	TRACE("ICMP received some data, buffer length %lu\n", buffer->size);

	net_domain* domain = get_domain(buffer);
	if (domain == NULL)
		return B_ERROR;

	NetBufferHeaderReader<icmp_header> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return bufferHeader.Status();

	icmp_header& header = bufferHeader.Data();
	uint8 type = header.type;

	TRACE("  got type %u, code %u, checksum %u\n", header.type, header.code,
		ntohs(header.checksum));
	TRACE("  computed checksum: %ld\n",
		gBufferModule->checksum(buffer, 0, buffer->size, true));

	if (gBufferModule->checksum(buffer, 0, buffer->size, true) != 0)
		return B_BAD_DATA;

	switch (type) {
		case ICMP_TYPE_ECHO_REPLY:
			deliver_echo_reply(buffer, header.echo.id);
			break;

		case ICMP_TYPE_ECHO_REQUEST:
		{
			net_domain* domain = get_domain(buffer);
			if (domain == NULL)
				break;

			if (buffer->interface_address != NULL) {
				// We only reply to echo requests of our local interface; we
				// don't reply to broadcast requests
				if (!domain->address_module->equal_addresses(
						buffer->interface_address->local, buffer->destination))
					break;
			}

			net_buffer* reply = gBufferModule->duplicate(buffer);
			if (reply == NULL)
				return B_NO_MEMORY;

			gBufferModule->swap_addresses(reply);

			// There already is an ICMP header, and we'll reuse it
			NetBufferHeaderReader<icmp_header> newHeader(reply);

			newHeader->type = type == ICMP_TYPE_ECHO_REPLY;
			newHeader->code = 0;
			newHeader->checksum = 0;

			newHeader.Sync();

			*ICMPChecksumField(reply) = gBufferModule->checksum(reply, 0,
					reply->size, true);

			status_t status = domain->module->send_data(NULL, reply);
			if (status < B_OK) {
				gBufferModule->free(reply);
				return status;
			}
			break;
		}

		case ICMP_TYPE_UNREACH:
		case ICMP_TYPE_SOURCE_QUENCH:
		case ICMP_TYPE_PARAMETER_PROBLEM:
		case ICMP_TYPE_TIME_EXCEEDED:
		case ICMP_TYPE_REDIRECT:
		{
			net_domain* domain = get_domain(buffer);
			if (domain == NULL)
				break;

			net_error error = icmp_to_net_error(header.type, header.code);
			if (error == 0)
				break;

			net_error_data dataStorage = {};
			net_error_data* data = NULL;
			if (error == B_NET_ERROR_MESSAGE_SIZE) {
				data = &dataStorage;
				data->mtu = ntohs(header.path_mtu.next_mtu);

				// IPv4 minimum fragment size is 68 bytes, so if the "next MTU" is
				// smaller than that, we can be sure it's invalid.
				if (data->mtu < 68)
					data = NULL;
			} else if (error == B_NET_ERROR_REDIRECT_HOST) {
				data = &dataStorage;
				sockaddr_in& gateway = (sockaddr_in&)data->gateway;
				gateway.sin_len = sizeof(sockaddr_in);
				gateway.sin_family = AF_INET;
				gateway.sin_addr.s_addr = header.redirect.gateway;
			}

			// Deliver the error to the domain protocol which will
			// propagate the error to the upper protocols
			bufferHeader.Remove();
			return domain->module->error_received(error, data, buffer);
		}

		case ICMP_TYPE_TIMESTAMP_REQUEST:
		case ICMP_TYPE_TIMESTAMP_REPLY:
		case ICMP_TYPE_INFO_REQUEST:
		case ICMP_TYPE_INFO_REPLY:
		default:
			// RFC 1122 3.2.2:
			// Unknown ICMP messages are silently discarded
			dprintf("ICMP: received unhandled type %u, code %u\n", header.type,
				header.code);
			break;
	}

	gBufferModule->free(buffer);
	return B_OK;
}


status_t
icmp_error_received(net_error code, net_error_data* errorData, net_buffer* data)
{
	return B_ERROR;
}


/*!	Sends an ICMP error message to the source of the \a buffer causing the
	error.
*/
status_t
icmp_error_reply(net_protocol* protocol, net_buffer* buffer, net_error error,
	net_error_data* errorData)
{
	TRACE("icmp_error_reply(code %s)\n", net_error_to_string(error));

	uint8 icmpType = 0;
	uint8 icmpCode = 0;
	net_error_to_icmp(error, icmpType, icmpCode);

	TRACE("  icmp type %u, code %u\n", icmpType, icmpCode);

	ipv4_header header;
	if (gBufferModule->restore_header(buffer, 0, &header, sizeof(ipv4_header))
			!= B_OK)
		return B_BAD_VALUE;

	// Check if we actually have an IPv4 header now
	if (header.version != IPV4_VERSION
		|| header.HeaderLength() < sizeof(ipv4_header)) {
		TRACE("  no IPv4 header found\n");
		return B_BAD_VALUE;
	}

	// RFC 1122 3.2.2:
	// ICMP error message should not be sent on reception of
	// an ICMP error message,
	if (header.protocol == IPPROTO_ICMP) {
		uint8 type;
		if (gBufferModule->restore_header(buffer, header.HeaderLength(), &type,
				1) != B_OK || is_icmp_error(type))
			return B_ERROR;
	}

	// a datagram to an IP multicast or broadcast address,
	if ((buffer->msg_flags & (MSG_BCAST | MSG_MCAST)) != 0)
		return B_ERROR;

	// a non-initial fragment
	if ((header.FragmentOffset() & IP_FRAGMENT_OFFSET_MASK) != 0)
		return B_ERROR;

	net_buffer* reply = gBufferModule->create(256);
	if (reply == NULL)
		return B_NO_MEMORY;

	if (buffer->destination->sa_family == AF_INET) {
		memcpy(reply->source, buffer->destination, buffer->destination->sa_len);
		memcpy(reply->destination, buffer->source, buffer->source->sa_len);
	} else {
		fill_sockaddr_in((sockaddr_in*)reply->source, header.destination);
		fill_sockaddr_in((sockaddr_in*)reply->destination, header.source);
	}

	// Now prepare the ICMP header

	NetBufferPrepend<icmp_header> icmpHeader(reply);
	icmpHeader->type = icmpType;
	icmpHeader->code = icmpCode;
	icmpHeader->zero = 0;
	icmpHeader->checksum = 0;

	if (errorData != NULL) {
		switch (error) {
			case B_NET_ERROR_REDIRECT_HOST:
			{
				sockaddr_in& gateway = (sockaddr_in&)errorData->gateway;
				icmpHeader->redirect.gateway = gateway.sin_addr.s_addr;
				break;
			}
			case B_NET_ERROR_PARAMETER_PROBLEM:
				icmpHeader->parameter_problem.pointer = errorData->error_offset;
				break;
			case B_NET_ERROR_MESSAGE_SIZE:
				icmpHeader->path_mtu.next_mtu = htons(errorData->mtu);
				break;

			default:
				break;
		}
	}

	icmpHeader.Sync();

	// Append IP header + 8 byte of the original datagram
	status_t status = gBufferModule->append_restored_header(reply, buffer, 0,
		std::min(header.HeaderLength() + 8, (int)header.TotalLength()));
	if (status == B_OK) {
		net_domain* domain = get_domain(buffer);
		if (domain == NULL)
			return B_ERROR;

		*ICMPChecksumField(reply)
			= gBufferModule->checksum(reply, 0, reply->size, true);

		reply->protocol = IPPROTO_ICMP;

		TRACE("  send ICMP message %p to %s\n", reply, AddressString(
				domain->address_module, reply->destination, true).Data());

		status = domain->module->send_data(NULL, reply);
	}
	if (status != B_OK)
		gBufferModule->free(reply);

	return status;
}


//	#pragma mark -


static status_t
icmp_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			new(&sPingSockets) PingSocketList;

			gStackModule->register_domain_protocols(AF_INET, SOCK_DGRAM,
				IPPROTO_ICMP,
				"network/protocols/icmp/v1",
				"network/protocols/ipv4/v1",
				NULL);

			gStackModule->register_domain_receiving_protocol(AF_INET,
				IPPROTO_ICMP, "network/protocols/icmp/v1");
			return B_OK;
		}

		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}


net_protocol_module_info sICMPModule = {
	{
		"network/protocols/icmp/v1",
		0,
		icmp_std_ops
	},
	NET_PROTOCOL_ATOMIC_MESSAGES,

	icmp_init_protocol,
	icmp_uninit_protocol,
	icmp_open,
	icmp_close,
	icmp_free,
	icmp_connect,
	icmp_accept,
	icmp_control,
	icmp_getsockopt,
	icmp_setsockopt,
	icmp_bind,
	icmp_unbind,
	icmp_listen,
	icmp_shutdown,
	icmp_send_data,
	icmp_send_routed_data,
	icmp_send_avail,
	icmp_read_data,
	icmp_read_avail,
	icmp_get_domain,
	icmp_get_mtu,
	icmp_receive_data,
	NULL,		// deliver_data()
	icmp_error_received,
	icmp_error_reply,
	NULL,		// add_ancillary_data()
	NULL,		// process_ancillary_data()
	NULL,		// process_ancillary_data_no_container()
	NULL,		// send_data_no_buffer()
	NULL		// read_data_no_buffer()
};

module_dependency module_dependencies[] = {
	{NET_STACK_MODULE_NAME, (module_info**)&gStackModule},
	{NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule},
	{}
};

module_info* modules[] = {
	(module_info*)&sICMPModule,
	NULL
};
