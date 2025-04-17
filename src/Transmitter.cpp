// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
// 
// See the License for the specific language governing permissions and limitations
// under the License.
//
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <cstdio>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include "spdlog/spdlog.h"
#include "Transmitter.h"
#include "IpSec.h"

static void create_udp_pkt( char *udp_buffer, const boost::asio::ip::udp::endpoint &endpoint, const char *data, size_t data_len,
                            const boost::asio::ip::address &local_address );
static void create_ip_hdr( char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size,
                           const boost::asio::ip::address &local_address );
static uint16_t calculate_sum( uint16_t *buffer, size_t len );

LibFlute::Transmitter::Transmitter ( const std::string& address, short port,
                                     uint64_t tsi, unsigned short mtu, uint32_t rate_limit,
                                     boost::asio::io_service& io_service,
                                     const std::optional<boost::asio::ip::udp::endpoint> &tunnel_endpoint )
    : _endpoint(boost::asio::ip::address::from_string(address), port)
    , _socket(io_service, _endpoint.protocol())
    , _io_service(io_service)
    , _send_timer(io_service)
    , _fdt_timer(io_service)
    , _tsi(tsi)
    , _mtu(mtu)
    , _files()
    , _files_mutex()
    , _mcast_address(address)
    , _rate_limit(rate_limit)
    , _tunnel_endpoint(tunnel_endpoint)
    , _tunnel_local_address()
{
  _max_payload = mtu -
    20 - // IPv4 header
     8 - // UDP header
    32 - // ALC Header with EXT_FDT and EXT_FTI
     4;  // SBN and ESI for compact no-code FEC
  if (_tunnel_endpoint.has_value()) {
    // Remove extra overhead for UDP tunnelling, if set
    _max_payload -= 20 - // IPv4 header
                    8; // UDP header
    boost::asio::ip::udp::socket local_socket(io_service, _tunnel_endpoint.value().protocol());
    local_socket.connect(_tunnel_endpoint.value());
    _tunnel_local_address = local_socket.local_endpoint().address();
  }
  uint32_t max_source_block_length = 64;

  _socket.set_option(boost::asio::ip::multicast::enable_loopback(true));
  _socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));

  _fec_oti = FecOti{FecScheme::CompactNoCode, 0, _max_payload, max_source_block_length};
  _fdt = std::make_unique<FileDeliveryTable>(1, _fec_oti);

  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));

  send_next_packet();
}

LibFlute::Transmitter::~Transmitter() = default;

auto LibFlute::Transmitter::enable_ipsec(uint32_t spi, const std::string& key) -> void
{
  LibFlute::IpSec::enable_esp(spi, _mcast_address, LibFlute::IpSec::Direction::Out, key);
}

auto LibFlute::Transmitter::handle_send_to(const boost::system::error_code& error) -> void
{
  if (!error) {
  }
}

auto LibFlute::Transmitter::seconds_since_epoch() -> uint64_t
{
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() +
      2'208'988'800; /* add the difference in seconds between the Unix epoch (1 January 1970, 00:00:00 UTC)
                        and the NTP epoch (1 January 1900, 00:00:00 UTC) */
}

auto LibFlute::Transmitter::send_fdt() -> void {
  _fdt->set_expires(seconds_since_epoch() + _fdt_repeat_interval * 2);
  auto fdt = _fdt->to_string();
  auto file = std::make_shared<File>(
        0,
        _fec_oti,
        "",
        "",
        seconds_since_epoch() + _fdt_repeat_interval * 2,
        (char*)fdt.c_str(),
        fdt.length(),
        true);
  if (file) {
    file->set_fdt_instance_id( _fdt->instance_id() );
    spdlog::debug("Sending FDT instance {}:\n{}", _fdt->instance_id(), _fdt->to_string());
    _files.insert_or_assign(0, file);
  }
}

auto LibFlute::Transmitter::send(
    const std::string& content_location,
    const std::string& content_type,
    uint32_t expires,
    char* data,
    size_t length) -> uint16_t
{
  auto toi = _toi;
  _toi++;
  if (_toi == 0) _toi = 1; // clamp to >= 1 in case it wraps

  auto file = std::make_shared<File>(
        toi,
        _fec_oti,
        content_location,
        content_type,
        expires,
        data,
        length);

  _fdt->add(file->meta());
  send_fdt();
  _files.insert({toi, file});
  return toi;
}

auto LibFlute::Transmitter::fdt_send_tick() -> void
{
  send_fdt();
  _fdt_timer.expires_from_now(boost::posix_time::seconds(_fdt_repeat_interval));
  _fdt_timer.async_wait( boost::bind(&Transmitter::fdt_send_tick, this));
}

auto LibFlute::Transmitter::file_transmitted(uint32_t toi) -> void
{
  if (toi != 0) {
    _files.erase(toi);
    _fdt->remove(toi);
    send_fdt();

    if (_completion_cb) {
      _completion_cb(toi);
    }
  }
}

auto LibFlute::Transmitter::send_next_packet() -> void
{
  uint32_t bytes_queued = 0;

  if (_files.size()) {
    for (auto& file_m : _files) {
      auto file = file_m.second;

      if (file && !file->complete()) {
        auto symbols = file->get_next_symbols(_max_payload);

        if (symbols.size()) {
          for(const auto& symbol : symbols) {
            spdlog::debug("sending TOI {} SBN {} ID {}", file->meta().toi, symbol.source_block_number(), symbol.id() );
          }
          auto packet = std::make_shared<AlcPacket>(_tsi, file->meta().toi, file->meta().fec_oti, symbols, _max_payload, file->fdt_instance_id());
          bytes_queued += packet->size();

	  boost::asio::ip::udp::endpoint send_endpoint;
          char *data = nullptr;
          size_t data_size = 0;
	  if (_tunnel_endpoint) {
	    send_endpoint = _tunnel_endpoint.value();
	    data_size = packet->size() + 20 /* IP header */ + 8 /* UDP header */;
            data = new char[data_size];
	    create_udp_pkt(data+20, _endpoint, packet->data(), packet->size(), _tunnel_local_address);
	    create_ip_hdr(data, _endpoint, data_size, _tunnel_local_address);
	  } else {
            send_endpoint = _endpoint;
	    data = packet->data();
            data_size = packet->size();
          }
          _socket.async_send_to(
              boost::asio::buffer(data, data_size), send_endpoint,
              [file, symbols, packet, this](
                const boost::system::error_code& error,
                std::size_t bytes_transferred)
              {
                if (error) {
                  spdlog::debug("sent_to error: {}", error.message());
                } else {
                  file->mark_completed(symbols, !error);
                  if (file->complete()) {
                    file_transmitted(file->meta().toi);
                  }
                }
              });
          if (_tunnel_endpoint) {
	    delete[] data;
          }
        }
        break;
      }
    }
  }
  if (!bytes_queued) {
    _send_timer.expires_from_now(boost::posix_time::milliseconds(10));
    _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
  } else {
    if (_rate_limit == 0) {
      _io_service.post(boost::bind(&Transmitter::send_next_packet, this));
    } else {
      auto send_duration = ((bytes_queued * 8.0) / (double)_rate_limit/1000.0) * 1000.0 * 1000.0;
      spdlog::trace("Rate limiter: queued {} bytes, limit {} kbps, next send in {} us",
          bytes_queued, _rate_limit, send_duration);
      _send_timer.expires_from_now(boost::posix_time::microseconds(
            static_cast<int>(ceil(send_duration))));
      _send_timer.async_wait( boost::bind(&Transmitter::send_next_packet, this));
    }
  }
}

static void create_udp_pkt(char *udp_buffer, const boost::asio::ip::udp::endpoint &endpoint, const char *data, size_t data_len, const boost::asio::ip::address &local_address)
{
  struct udp_pseudo_hdr {
    in_addr_t source;
    in_addr_t dest;
    uint8_t reserved;
    uint8_t protocol;
    uint16_t length;
  } *pseudo_hdr = reinterpret_cast<struct udp_pseudo_hdr*>(udp_buffer - sizeof(*pseudo_hdr));
  struct udphdr *udp_hdr = reinterpret_cast<struct udphdr*>(udp_buffer);

  pseudo_hdr->source = htonl(local_address.to_v4().to_ulong());
  pseudo_hdr->dest = htonl(endpoint.address().to_v4().to_ulong());
  pseudo_hdr->reserved = 0;
  pseudo_hdr->protocol = endpoint.protocol().protocol();
  pseudo_hdr->length = htons(data_len + 8);

  udp_hdr->uh_sport = htons(endpoint.port());
  udp_hdr->uh_dport = udp_hdr->uh_sport;
  udp_hdr->uh_ulen = pseudo_hdr->length;
  udp_hdr->uh_sum = 0;
  memcpy(udp_buffer+8, data, data_len);

  udp_hdr->uh_sum = calculate_sum(reinterpret_cast<uint16_t*>(pseudo_hdr), data_len + 8 + 12);
}

static void create_ip_hdr(char *ip_buffer, const boost::asio::ip::udp::endpoint &endpoint, size_t pkt_size, const boost::asio::ip::address &local_address)
{
  struct iphdr *ip_hdr = reinterpret_cast<struct iphdr*>(ip_buffer);

  ip_hdr->version = IPVERSION;
  ip_hdr->ihl = 5; // 20 bytes
  ip_hdr->tos = 0;
  ip_hdr->tot_len = htons(pkt_size);
  ip_hdr->id = 0;
  ip_hdr->frag_off = 0; // not fragmenting
  ip_hdr->ttl = 63; // TTL 63 hops
  ip_hdr->protocol = endpoint.protocol().protocol();
  ip_hdr->check = 0;
  ip_hdr->saddr = htonl(local_address.to_v4().to_ulong());
  ip_hdr->daddr = htonl(endpoint.address().to_v4().to_ulong());

  ip_hdr->check = calculate_sum(reinterpret_cast<uint16_t*>(ip_hdr), 20);
}

static uint16_t calculate_sum(uint16_t *buffer, size_t len)
{
    uint32_t cksum = 0;

    while (len > 1) {
	cksum += ntohs(*buffer);
        len -= 2;
        buffer++;
    }
    if (len > 0) {
        cksum = *reinterpret_cast<uint8_t*>(buffer);
    }
    uint16_t result = ~htons(static_cast<uint16_t>(cksum & 0xFFFF) + static_cast<uint16_t>(cksum >> 16));

    return result;
}
