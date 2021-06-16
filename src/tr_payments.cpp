/*

Copyright (c) 2021, libtorrent project
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/config.hpp"
#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/peer_connection_handle.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/http_parser.hpp"
#include "libtorrent/http_connection.hpp"
#include "libtorrent/resolver.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/broadcast_socket.hpp"
#include "libtorrent/socket_io.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/aux_/socket_type.hpp" // for is_utp
#include "libtorrent/performance_counters.hpp" // for counters
#include "libtorrent/request_blocks.hpp" // for request_a_block
#include "libtorrent/extensions/tr_payments.hpp"
#include "libtorrent/aux_/time.hpp"
#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/json/src.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"
#include <cstring>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>

#ifndef TORRENT_DISABLE_EXTENSIONS

#if 0
#define paym_log(msg) printf("%s\n", msg)
#else
#define paym_log(msg)
#endif

namespace libtorrent { namespace {

	class node_interface {
		public:
			virtual ~node_interface() {}
			// return success if correctly configured
			virtual bool test_connection() = 0;
			// create invoice with this description. msats is the amount of sats we request per piece
			// return: payment hash, bolt11
			// payment hash for checking later if the invoice has been paid
			// bolt11 for sending to peer
			virtual std::tuple<std::string, std::string> create_invoice(const std::string& desc, int msats) = 0;
			// check if the requested amount is within what we can pay
			// return: if affordable, return the description of this invoice, else empty string
			virtual std::string decode_invoice(const string_view& bolt11, int can_pay_msat) = 0;
			// pay invoice
			// return: true if success
			virtual bool pay_invoice(const string_view& bolt11) = 0;
			// return: the amount of msat received with this payment hash
			virtual long long check_invoice(const std::string& hash) = 0;
	};

	class clightning : public node_interface {
		public:
		clightning(const std::string& rpc_name)
			: m_lightning_rpc_filename(rpc_name)
			, m_fd(-1)
			, m_next_id(0)
		{
		}

		~clightning()
		{
			ln_disconnect();
		}

		bool test_connection()
		{
			ln_connect();
			return m_fd > 0;
		}

		std::tuple<std::string, std::string> create_invoice(const std::string& desc, int msats)
		{
			ln_connect();
			char label[32];
			for(int i = 0; i < 31; i++) {
				label[i] = static_cast<char>('a' + random(16));
			}
			label[31] = 0;
			boost::json::object obj;
			obj["description"] = desc;
			obj["msatoshi"] = msats;
			obj["label"] = std::string(label);
			obj["expiry"] = 900;
			auto res = send_lightning("invoice", obj);

			if(!res.contains("payment_hash") ||
					!res.contains("bolt11"))
				return std::tuple<std::string, std::string>("", "");

			auto descv = res.if_contains("payment_hash");
			auto hash = descv->if_string();
			if(!hash) return std::tuple<std::string, std::string>("", "");
			auto boltv = res.if_contains("bolt11");
			auto bolt = boltv->if_string();
			if(!bolt) return std::tuple<std::string, std::string>("", "");

			return std::make_tuple(std::string(hash->c_str()), std::string(bolt->c_str()));
		}

		std::string decode_invoice(const string_view& bolt11, int can_pay_msat)
		{
			ln_connect();
			boost::json::object obj;
			obj["bolt11"] = bolt11;
			auto res = send_lightning("decodepay", obj);
			if(!res.contains("msatoshi") ||
					!res.contains("payee") ||
					!res.contains("description") ||
					!res.contains("currency")) {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Unable to decode invoice");
#endif
				return "";
			}

			auto msatv = res.if_contains("msatoshi");
			auto msat = msatv->if_int64();
			if(!msat) return "";
			if(*msat > can_pay_msat) return "";
			auto descv = res.if_contains("description");
			auto desc = descv->if_string();
			if(!desc) return "";
			return std::string(desc->c_str());
		}

		bool pay_invoice(const string_view& bolt11)
		{
			ln_connect();
			using namespace boost::json;
			object obj;
			obj["bolt11"] = bolt11;
			auto res = send_lightning("pay", obj);
			auto statv = res.if_contains("status");
			if(!statv) return -1;
			return *statv == "complete";
		}

		long long check_invoice(const std::string& hash)
		{
			ln_connect();
			boost::json::object obj;
			obj["payment_hash"] = hash;
			auto res = send_lightning("listinvoices", obj);
			if(!res.contains("invoices")) {
				return 0;
			}

			auto invlistv = res.if_contains("invoices");
			if(!invlistv) return 0;
			auto invlist = invlistv->if_array();
			if(!invlist) return 0;
			if(invlist->size() != 1) return false;
			auto invv = invlist->at(0);
			auto inv = invv.if_object();
			if(!inv) return 0;
			auto paidv = inv->if_contains("status");
			if(!paidv) return 0;
			auto paid = paidv->if_string();
			if(!paid) return 0;
			if(*paid != boost::json::string("paid")) return 0;
			auto sumv = inv->if_contains("msatoshi_received");
			if(!sumv) return 0;
			auto sum = sumv->if_int64();
			if(!sum) return 0;
			return *sum;
		}


	private:
		std::string m_lightning_rpc_filename;
		int m_fd;
		int m_next_id;

		void ln_connect()
		{
			if(m_fd > 0)
				return;
			struct sockaddr_un addr;
			int fd = socket(AF_UNIX, SOCK_STREAM, 0);
			std::strcpy(addr.sun_path, m_lightning_rpc_filename.c_str());
			addr.sun_family = AF_UNIX;

			if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
			{   
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Unable to connect to node");
#endif
				return;
			}

			// we'll do blocking reads but with a 1s timeout
			// TODO: Windows support
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 1000000;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

			m_fd = fd;
		}

		void ln_disconnect()
		{
			if(m_fd == -1)
				return;
			close(m_fd);
			m_fd = -1;
		}

		boost::json::object send_lightning(const std::string& method, const boost::json::object& params)
		{
			using namespace boost::json;
			object obj;
			obj["jsonrpc"] = "2.0";
			obj["id"] = m_next_id;
			obj["method"] = method;
			obj["params"] = params;
			std::string out = serialize(obj);
			ssize_t written = write(m_fd, out.c_str(), static_cast<unsigned int>(out.length()));
			if(written != static_cast<int>(out.length()))
				return object();
			m_next_id++;

			char rbuf[4096];
			memset(rbuf, 0, sizeof(rbuf));
			if (read(m_fd, rbuf, sizeof(rbuf)) < 0)
			{
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Unable to read data from node");
#endif
				return object();
			}

			error_code ec;
			value jv = parse(rbuf, ec);
			if(ec) {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Error parsing JSON");
#endif
				return object();
			}

			auto jv2 = jv.if_object();
			if(!jv2) return object();
			auto idv = jv2->if_contains("id");
			if(!idv) return object();
			auto id = idv->if_int64();
			if(*id != m_next_id - 1) return object();

			if(jv2->contains("error") || !jv2->contains("result")) {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Invalid content in JSON");
#endif
				return object();
			}
			auto resv = jv2->if_contains("result");
			if(!resv) return object();
			auto res = resv->if_object();
			if(!res) return object();
			return *res;
		}

	};

	class lnd_node : public node_interface {
		public:
		lnd_node(const std::string& macaroon_path, const std::string& tls_path, const std::string& hostname, const std::string& port)
			: m_macaroon_path(macaroon_path)
			, m_tls_path(tls_path)
			, m_hostname(hostname)
			, m_port(std::stoi(port))
#ifdef TORRENT_USE_OPENSSL
			, m_ssl_ctx(ssl::context::method::sslv23_client)
#endif
		{
			using namespace boost::asio;
#ifdef TORRENT_USE_OPENSSL
			m_ssl_ctx.set_verify_mode(boost::asio::ssl::context::verify_none);
#endif
			ip::tcp::resolver resolver(m_io_service);
			m_ip = resolver.resolve(tcp::endpoint(ip::address::from_string(m_hostname), ip::port_type(m_port)));

			std::ifstream file(m_macaroon_path, std::ios::binary | std::ios::ate);
			std::streamsize size = file.tellg();
			file.seekg(0, std::ios::beg);
			std::vector<char> buf(size);
			if (file.read(buf.data(), size))
			{
				for (auto i = 0; i < buf.size(); ++i) {
					char rep[3];
					snprintf(rep, sizeof(rep), "%02X", static_cast<unsigned char>(buf[i]));
					m_macaroon_contents.push_back(rep[0]);
					m_macaroon_contents.push_back(rep[1]);
				}
			} else {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Unable to read macaroon");
#endif
			}

		}

		~lnd_node()
		{
		}

		bool test_connection()
		{
			auto res = http_get("/v1/getinfo");
			auto peersv = res.if_contains("num_peers");
			if(!peersv) return false;
			auto peers = peersv->if_int64();
			if(!peers) return false;
			return *peers > 0;
		}

		std::tuple<std::string, std::string> create_invoice(const std::string& desc, int msats)
		{
			boost::json::object obj;
			obj["memo"] = desc;
			obj["value_msat"] = msats;
			auto res = http_post("/v1/invoices", boost::json::serialize(obj));
			auto boltv = res.if_contains("payment_request");
			if(!boltv) return std::tuple<std::string, std::string>("", "");
			auto bolt = boltv->if_string();
			if(!bolt) return std::tuple<std::string, std::string>("", "");

			char path[4096];
			snprintf(path, sizeof(path), "/v1/payreq/%s", bolt->c_str());
			auto res2 = http_get(path);
			auto hashv = res2.if_contains("payment_hash");
			if(!hashv) return std::tuple<std::string, std::string>("", "");
			auto hash = hashv->if_string();
			if(!hash) return std::tuple<std::string, std::string>("", "");
			return std::make_tuple(std::string(hash->c_str()), std::string(bolt->c_str()));
		}

		std::string decode_invoice(const string_view& bolt11, int can_pay_msat)
		{
			char path[4096];
			snprintf(path, sizeof(path), "/v1/payreq/%s", std::string(bolt11).c_str());
			auto res = http_get(path);
			auto valv = res.if_contains("num_msat");
			if(!valv) return "";
			int msat = -1;
			auto val = valv->if_int64();
			if(val) {
				msat = *val;
			} else {
				auto val2 = valv->if_string();
				if(val2)
					msat = std::stoi(std::string(val2->c_str()));
			}
			if(msat == -1) return "";
			if(msat > can_pay_msat) return "";
			auto memov = res.if_contains("description");
			if(!memov) return "";
			auto memo = memov->if_string();
			if(!memo) return "";
			return std::string(memo->c_str());
		}

		bool pay_invoice(const string_view& bolt11)
		{
			boost::json::object obj;
			obj["payment_request"] = std::string(bolt11).c_str();
			auto res = http_post("/v1/channels/transactions", boost::json::serialize(obj));
			auto errv = res.if_contains("payment_error");
			if(!errv) return false;
			auto err = errv->if_string();
			if(!err) return false;
			return *err == "";
		}

		long long check_invoice(const std::string& hash)
		{
			char path[4096];
			snprintf(path, sizeof(path), "/v1/invoice/%s", hash.c_str());
			auto res = http_get(path);
			auto valv = res.if_contains("amt_paid_msat");
			if(!valv) return 0;
			auto val = valv->if_int64();
			if(val) return *val;
			// seems to be sent as a string?
			auto val2 = valv->if_string();
			if(val2) return std::stoi(std::string(val2->c_str()));
			return 0;
		}


	private:
		mutable std::shared_ptr<http_connection> m_connection;
		io_service m_io_service;
		std::string m_macaroon_path;
		std::string m_tls_path;
		std::string m_hostname;
		int m_port;
#ifdef TORRENT_USE_OPENSSL
		boost::asio::ssl::context m_ssl_ctx;
#endif
		std::string m_macaroon_contents;
		tcp::resolver::results_type m_ip;

		boost::json::object http_do(const std::string& path, const std::string& verb, const std::string& data)
		{
			using namespace boost::asio;

			// TODO should really make this async so we can have a 1ms timeout
			boost::asio::ssl::stream<ip::tcp::socket> sock(m_io_service, m_ssl_ctx);
			boost::asio::connect(sock.lowest_layer(), m_ip);
#ifdef TORRENT_USE_OPENSSL
			sock.handshake(ssl::stream_base::handshake_type::client);
#endif
			char request[4096];
			char conlen[1024];
			memset(conlen, 0, sizeof(conlen));
			if(data.size())
				snprintf(conlen, sizeof(conlen), "Content-Length: %lu\r\n", data.size());
			snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\n"
					"Host: %s:%d\r\n"
					"User-Agent: libtorrent/" LIBTORRENT_VERSION "\r\n"
					"Accept: */*\r\n"
					"Connection: close\r\n"
					"%s"
					"Grpc-Metadata-macaroon: %s\r\n\r\n%s", verb.c_str(), path.c_str(), m_hostname.c_str(), m_port, conlen, m_macaroon_contents.c_str(), data.c_str());
			boost::asio::write(sock, boost::asio::buffer(request));

			std::string response;

			boost::system::error_code ec;
			do {
				char indata[1024];
				size_t bytes_transferred = sock.read_some(boost::asio::buffer(indata, sizeof(indata)), ec);
				if (!ec) response.append(indata, indata + bytes_transferred);
			} while(!ec);

			std::istringstream respstream(response);
			std::string line;
			std::getline(respstream, line);
			int content_length = 0;
			if(line == "HTTP/1.1 200 OK\r") {
				while(std::getline(respstream, line)) {
					if(line.rfind("Content-Length: ", 0) == 0) {
						content_length = stoi(line.substr(16, std::string::npos));
					}
					if(!line.empty() && line[0] == '{') {
						boost::json::value jv = boost::json::parse(line.substr(0, content_length), ec);
						auto jv2 = jv.if_object();
						if(jv2) {
							return *jv2;
						}
					}
				}
			}
			return boost::json::object();
		}

		boost::json::object http_post(const std::string& path, const std::string& data)
		{
			return http_do(path, "POST", data);
		}

		boost::json::object http_get(const std::string& path)
		{
			return http_do(path, "GET", "");
		}
	};

	const char extension_name[] = "tr_payments";

	enum
	{
		extension_index = 5,
	};

	struct tr_payments_plugin final
		: plugin
	{
		tr_payments_plugin(const payment_node_configuration& config)
			: m_node(nullptr)
		{
			if(config.name == "clightning") {
				if(config.options.size() != 1)
					return;
				m_node = std::make_unique<clightning>(config.options[0]);
			}
			else if(config.name == "lnd") {
				if(config.options.size() != 4)
					return;
				m_node = std::make_unique<lnd_node>(config.options[0],
						config.options[1],
						config.options[2],
						config.options[3]);
			} // TODO more node implementations

			if(m_node && !m_node->test_connection()) {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Unable to connect to node");
#endif
				m_node.release();
			}
		}

		~tr_payments_plugin()
		{
			abort();
		}

		void added (session_handle const&)
		{
		}

		void abort ()
		{
		}

		std::unique_ptr<node_interface> m_node;

		std::shared_ptr<torrent_plugin> new_torrent (torrent_handle const& th, void* cd);

		// returns info of the piece that was paid for
		std::optional<peer_request> decode_and_pay_invoice(const string_view& bolt11, const torrent& t, int can_pay)
		{
			if(!m_node)
				return {};
			std::string desc = m_node->decode_invoice(bolt11, can_pay);
			if(desc.empty()) return {};
			peer_request r;
			std::stringstream ss;
			int piece;
			ss << desc.c_str();
			ss >> piece;
			r.piece = piece;
			ss >> r.start;
			ss >> r.length;
			bool fail = ss.fail();
			if(fail)
				return {};

			// this is copy-pasted from peer_connection.cpp::incoming_have...
			if (!t.has_piece_passed(r.piece)
				&& !t.is_upload_only()
				&& (!t.has_picker() || t.picker().piece_priority(r.piece) != dont_download)) {
				// NOTE we may receive and pay multiple invoices if
				// there are many requests in flight. This will
				// probably even out over time.
				bool succ = m_node->pay_invoice(bolt11);
				if(!succ) return {};
				return r;
			}
			return {};
		}

		std::tuple<std::string, std::string> create_invoice(const peer_request& pr, int msats)
		{
			if(!m_node)
				return {};
			std::stringstream ss;
			ss << pr.piece << " " << pr.start << " " << pr.length;
			return m_node->create_invoice(ss.str(), msats);
		}

		long long check_invoice(const std::string& hash)
		{
			if(!m_node)
				return 0;
			return m_node->check_invoice(hash);
		}

	private:

	};

	struct tr_payments_torrent_plugin final
		: torrent_plugin
	{
		explicit tr_payments_torrent_plugin (torrent& t, payment_options* po, tr_payments_plugin& pp)
			: m_torrent(t)
			, m_pp(pp)
			, m_req_price_msat(0)
			, m_can_pay_msat(0)
		{
			int num_pieces = m_torrent.torrent_file().num_pieces();
			m_req_price_msat = po->m_req_total_price_msat / num_pieces;
			m_can_pay_msat = po->m_can_pay_total_price_msat / num_pieces;
			delete po;
		}

		std::shared_ptr<peer_plugin> new_connection (peer_connection_handle const& pc);

		long long check_invoice(const std::string& hash)
		{
			return m_pp.check_invoice(hash);
		}

		// returns index of the piece that was paid for
		std::optional<peer_request> decode_and_pay_invoice(const string_view& bolt11, const torrent& t)
		{
			if(!m_can_pay_msat)
				return {};
			return m_pp.decode_and_pay_invoice(bolt11, t, m_can_pay_msat);
		}

		std::tuple<std::string, std::string> create_invoice(const peer_request& pr, int msats)
		{
			if(!m_req_price_msat)
				return std::tuple<std::string, std::string>("", "");
			return m_pp.create_invoice(pr, msats);
		}

	private:
		torrent& m_torrent;
		tr_payments_plugin& m_pp;
		int m_req_price_msat;
		int m_can_pay_msat;
		// explicitly disallow assignment, to silence msvc warning
		tr_payments_torrent_plugin& operator=(tr_payments_torrent_plugin const&) = delete;
	};

	struct tr_payments_peer_plugin final
		: peer_plugin
	{
		tr_payments_peer_plugin(torrent& t, peer_connection& pc, tr_payments_torrent_plugin& tp, int req_price_msat, int can_pay_msat)
			: m_torrent(t)
			, m_pc(pc)
			, m_tp(tp)
			, m_sent_freebie(false)
			, m_already_received_balance_msat(0)
			, m_requested_price_per_piece(req_price_msat)
			, m_can_pay_per_piece(can_pay_msat)
			, m_message_index(0)
		{
		}

		void add_handshake(entry& h) override
		{
			if(!m_can_pay_per_piece && !m_requested_price_per_piece)
				return;
			entry& messages = h["m"];
			messages[extension_name] = extension_index;
		}

		bool on_extension_handshake(bdecode_node const& h) override
		{
			m_message_index = 0;
			if (h.type() != bdecode_node::dict_t) return false;
			bdecode_node const messages = h.dict_find_dict("m");
			if (!messages) return false;

			int const index = int(messages.dict_find_int_value(extension_name, -1));
			if (index == -1) return false;
			m_message_index = index;
#ifndef TORRENT_DISABLE_LOGGING
			paym_log("Received valid ext handshake");
#endif
			return true;
		}

		bool on_extended(int const length, int const msg, span<char const> body) override
		{
			// this is where we receive an invoice
			if (msg != extension_index) return false;
			if (length > 1024) {
				// TODO pex error
				m_pc.disconnect(errors::pex_message_too_large, operation_t::bittorrent, peer_connection_interface::peer_error);
				return true;
			}
			if (int(body.size()) < length) return true;

			bdecode_node pay_msg;
			error_code ec;
			int const ret = bdecode(body.begin(), body.end(), pay_msg, ec);
			if (ret != 0 || pay_msg.type() != bdecode_node::dict_t || ec)
			{
				// TODO pex error
				m_pc.disconnect(errors::invalid_pex_message, operation_t::bittorrent, peer_connection_interface::peer_error);
				return true;
			}

			bdecode_node p = pay_msg.dict_find_string("bolt11");

			auto pr = m_tp.decode_and_pay_invoice(p.string_value(), m_torrent);
			if(!pr.has_value())
				return true;

			char buf[17];
			char* ptr = buf;
			namespace io = detail;
			io::write_uint32(13, ptr);
			io::write_uint8(bt_peer_connection::msg_request, ptr);
			io::write_uint32(static_cast<int>(pr->piece), ptr);
			io::write_uint32(pr->start, ptr);
			io::write_uint32(pr->length, ptr);
			m_pc.send_buffer({buf, 17});
			return true;
		}

		bool on_request (peer_request const& pr) override
		{
			if(!m_sent_freebie) {
				m_sent_freebie = true;
				return false;
			}
			if(m_requested_price_per_piece == 0)
				return false;
			// check invoice status
			// update balance
			check_invoices();
			// check if enough balance
			// if enough balance, return false
			int request_cost = m_requested_price_per_piece * pr.length / m_torrent.torrent_file().piece_length();
			if(m_already_received_balance_msat >= request_cost) {
				m_already_received_balance_msat -= request_cost;
				return false;
			}
			// if not enough balance, request payment, return true
			auto tup = m_tp.create_invoice(pr, m_requested_price_per_piece);
			if(std::get<0>(tup).empty()) {
#ifndef TORRENT_DISABLE_LOGGING
				paym_log("Could not create invoice");
#endif
				return true;
			}
			send_invoice(std::get<1>(tup)); // send bolt11
			note_sent_invoice(std::get<0>(tup)); // note payment hash
			return true; // do not respond, awaiting payment
		}

	private:

		torrent& m_torrent;
		peer_connection& m_pc;
		tr_payments_torrent_plugin& m_tp;
		bool m_sent_freebie;
		int m_already_received_balance_msat;
		int m_requested_price_per_piece;
		int m_can_pay_per_piece;
		std::set<std::string> m_pending_payment_hashes;
		int m_message_index;

		// explicitly disallow assignment, to silence msvc warning
		tr_payments_peer_plugin& operator=(tr_payments_peer_plugin const&) = delete;

		void send_invoice(const std::string& bolt11)
		{
			entry pay;
			pay["bolt11"] = bolt11.c_str();
			std::vector<char> pay_msg;
			bencode(std::back_inserter(pay_msg), pay);
			char msg[6];
			char* ptr = msg;
			detail::write_uint32(1 + 1 + int(pay_msg.size()), ptr);
			detail::write_uint8(bt_peer_connection::msg_extended, ptr);
			detail::write_uint8(m_message_index, ptr);
			m_pc.send_buffer(msg);
			m_pc.send_buffer(pay_msg);
		}

		void note_sent_invoice(const std::string& hash)
		{
			m_pending_payment_hashes.insert(hash);
		}

		void check_invoices()
		{
			auto it = m_pending_payment_hashes.begin();
			while(it != m_pending_payment_hashes.end()) {
				long long paid = m_tp.check_invoice(*it);
				if(paid) {
					it = m_pending_payment_hashes.erase(it);
					m_already_received_balance_msat += paid;
				} else {
					it++;
				}
			}
		}
	};

	std::shared_ptr<peer_plugin> tr_payments_torrent_plugin::new_connection (peer_connection_handle const& pc)
	{
		if (pc.type() != connection_type::bittorrent) return {};

		bt_peer_connection* c = static_cast<bt_peer_connection*>(pc.native_handle().get());
		auto p = std::make_shared<tr_payments_peer_plugin>(m_torrent, *c, *this, m_req_price_msat, m_can_pay_msat);
		return p;
	}

	std::shared_ptr<torrent_plugin> tr_payments_plugin::new_torrent (torrent_handle const& th, void* cd)
	{
		payment_options* po = (payment_options*)cd;
		if(!po || (po->m_req_total_price_msat == 0 && po->m_can_pay_total_price_msat == 0))
			return {};
		torrent* t = th.native_handle().get();
		return std::make_shared<tr_payments_torrent_plugin>(*t, po, *this);
	}
} }



namespace libtorrent {

	std::shared_ptr<plugin> create_tr_payments_plugin(const payment_node_configuration& config)
	{
		return std::make_shared<tr_payments_plugin>(config);
	}
}

#endif
