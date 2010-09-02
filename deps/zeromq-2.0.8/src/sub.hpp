/*
    Copyright (c) 2007-2010 iMatix Corporation

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ZMQ_SUB_HPP_INCLUDED__
#define __ZMQ_SUB_HPP_INCLUDED__

#include "../include/zmq.h"

#include "prefix_tree.hpp"
#include "socket_base.hpp"
#include "fq.hpp"

namespace zmq
{

    class sub_t : public socket_base_t
    {
    public:

        sub_t (class app_thread_t *parent_);
        ~sub_t ();

    protected:

        //  Overloads of functions from socket_base_t.
        void xattach_pipes (class reader_t *inpipe_, class writer_t *outpipe_,
            const blob_t &peer_identity_);
        void xdetach_inpipe (class reader_t *pipe_);
        void xdetach_outpipe (class writer_t *pipe_);
        void xkill (class reader_t *pipe_);
        void xrevive (class reader_t *pipe_);
        void xrevive (class writer_t *pipe_);
        int xsetsockopt (int option_, const void *optval_, size_t optvallen_);
        int xsend (zmq_msg_t *msg_, int flags_);
        int xrecv (zmq_msg_t *msg_, int flags_);
        bool xhas_in ();
        bool xhas_out ();

    private:

        //  Check whether the message matches at least one subscription.
        bool match (zmq_msg_t *msg_);

        //  Fair queueing object for inbound pipes.
        fq_t fq;

        //  The repository of subscriptions.
        prefix_tree_t subscriptions;

        //  If true, 'message' contains a matching message to return on the
        //  next recv call.
        bool has_message;
        zmq_msg_t message;

        //  If true, part of a multipart message was already received, but
        //  there are following parts still waiting.
        bool more;

        sub_t (const sub_t&);
        void operator = (const sub_t&);
    };

}

#endif
