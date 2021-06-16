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

#ifndef TORRENT_TR_PAYMENTS_EXTENSION_HPP_INCLUDED
#define TORRENT_TR_PAYMENTS_EXTENSION_HPP_INCLUDED

#ifndef TORRENT_DISABLE_EXTENSIONS

#include "libtorrent/config.hpp"

#include <memory>
#include <string>
#include <vector>

namespace libtorrent {

	struct plugin;
	struct torrent_handle;

	struct payment_options {
		// how many sats we want for the whole file
		int m_req_total_price_msat;
		// how many sats we're willing to pay for the whole file
		int m_can_pay_total_price_msat;
	};

	struct payment_node_configuration {
		// possible names:
		// clightning
		// lnd
		std::string name;
		// options for clightning:
		// must be 1 element: local path to rpc filename
		// options for lnd:
		// must be 4 elements:
		// - local path to admin.macaroon file
		// - local path to tls.cert file
		//   TODO: cert currently not used
		//   can be empty in which case tls isn't used for the calls to the node
		// - hostname for the REST RPC API
		// - port number for the REST RPC API
		std::vector<std::string> options;
	};

	// constructor function for the tr_payments extension. The tr_payments
	// extension allows peers to request and send payments.
	// This extension is enabled by default unless explicitly disabled in
	// the session constructor.
	//
	// This can either be passed in the add_torrent_params::extensions field, or
	// via torrent_handle::add_extension().
	TORRENT_EXPORT std::shared_ptr<plugin> create_tr_payments_plugin(const payment_node_configuration& config);
}

#endif // TORRENT_DISABLE_EXTENSIONS

#endif // TORRENT_TR_PAYMENTS_EXTENSION_HPP_INCLUDED
