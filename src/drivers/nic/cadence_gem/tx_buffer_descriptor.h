/*
 * \brief  Base EMAC driver for the Xilinx EMAC PS used on Zynq devices
 * \author Timo Wischer
 * \author Johannes Schlatow
 * \date   2015-03-10
 */

/*
 * Copyright (C) 2015-2018 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _INCLUDE__DRIVERS__NIC__CADENCE_GEM__TX_BUFFER_DESCRIPTOR_H_
#define _INCLUDE__DRIVERS__NIC__CADENCE_GEM__TX_BUFFER_DESCRIPTOR_H_

#include <base/log.h>
#include <cpu/cache.h>
#include <timer_session/connection.h>
#include <nic_session/nic_session.h>

#include "buffer_descriptor.h"

namespace Cadence_gem {
	using namespace Genode;

	template <typename SINK>
	class Tx_buffer_descriptor;
}


template <typename SINK>
class Cadence_gem::Tx_buffer_descriptor : public Buffer_descriptor
{
	private:
		enum { BUFFER_COUNT = 1024 };

		struct Addr : Register<0x00, 32> {};
		struct Status : Register<0x04, 32> {
			struct Length  : Bitfield<0, 14> {};
			struct Last_buffer  : Bitfield<15, 1> {};
			struct Wrap  : Bitfield<30, 1> {};
			struct Used  : Bitfield<31, 1> {};
			struct Chksum_err : Bitfield<20, 3> {};
			struct Crc_present: Bitfield<16, 1> {};
			struct Late_collision: Bitfield<26, 1> {};
			struct Corrupt: Bitfield<27, 1> {};
			struct Retry_limit: Bitfield<29, 1> {};
			struct Error : Bitfield<20,10> {};
		};

		SINK              &_sink;
		Timer::Connection &_timer;

		addr_t const _phys_base;
		addr_t const _virt_base;

		void _reset_descriptor(unsigned const i, addr_t phys_addr) {
			if (i > _max_index())
				return;

			/* set physical buffer address */
			_descriptors[i].addr   = phys_addr;

			/* set used by SW, also we do not use frame scattering */
			_descriptors[i].status = Status::Used::bits(1) |
			                         Status::Last_buffer::bits(1);

			/* last buffer must be marked by Wrap bit */
			if (i == _max_index())
				_descriptors[i].status |= Status::Wrap::bits(1);
		}

	public:

		class Package_send_timeout : public Genode::Exception {};

		Tx_buffer_descriptor(Genode::Env &env,
		                     SINK &sink,
		                     Timer::Connection &timer)
		: Buffer_descriptor(env, BUFFER_COUNT),
		  _sink(sink),
		  _timer(timer),
		  _phys_base(Dataspace_client(sink.dataspace()).phys_addr()),
		  _virt_base(env.rm().attach(sink.dataspace()))
		{
			for (size_t i=0; i <= _max_index(); i++) {
				/* configure all descriptors with address 0, which we
				 * interpret as invalid */
				_reset_descriptor(i, 0x0);
			}
		}

		void reset()
		{
			/* ack all packets that are still queued */
			submit_acks(true);

			/* reset head and tail */
			_reset_head();
			_reset_tail();
		}

		void submit_acks(bool force=false)
		{
			/* the tail marks the descriptor for which we wait to
			 * be handed over to software */
			for (size_t i=0; i < _queued(); i++) {
				/* stop if still in use by hardware */
				if (!Status::Used::get(_tail().status) && !force)
					break;

				/* if descriptor has been configured properly */
				if (_tail().addr != 0) {
					/* build packet descriptor from buffer descriptor
					 * and acknowledge packet */
					const size_t length = Status::Length::get(_tail().status);
					Nic::Packet_descriptor p((addr_t)_tail().addr - _phys_base, length);
					if (_sink.packet_valid(p))
						_sink.acknowledge_packet(p);
					else
						warning("Invalid packet descriptor");

					/* erase address so that we don't send an ack again */
					_tail().addr = 0;

					/* evaluate Tx status */
					if (Status::Retry_limit::get(_tail().status))
						warning("Retry limit exceeded");

					if (Status::Corrupt::get(_tail().status))
						warning("Transmit frame corruption");

					if (Status::Late_collision::get(_tail().status))
						warning("Late collision error");

					if (Status::Crc_present::get(_tail().status))
						warning("CRC already present - this impedes checksum offloading");

					if (Status::Chksum_err::get(_tail().status))
						warning("Tx checksum error: ", Status::Chksum_err::get(_tail().status));

					if (Status::Error::get(_tail().status))
						warning("Unknown error: ", Status::Error::get(_tail().status));
				}

				_advance_tail();
			}
		}

		void add_to_queue(Nic::Packet_descriptor p)
		{
			/* the head marks the descriptor that we use next for
			 * handing over the packet to hardware */
			if (p.size() > BUFFER_SIZE) {
				warning("Ethernet package to big. Not sent!");
				return;
			}

			addr_t const packet_phys = _phys_base + p.offset();
			addr_t const packet_virt = _virt_base + p.offset();

			if (packet_phys & 0x1f) {
				warning("Packet is not aligned properly.");
			}

			cache_clean_invalidate_data(packet_virt, p.size());

			/* wait until the used bit is set (timeout after 10ms) */
			uint32_t timeout = 10000;
			while ( !Status::Used::get(_head().status) ) {
				if (timeout == 0) {
					throw Package_send_timeout();
				}
				timeout -= 1000;

				/*  TODO buffer is full, instead of sleeping we should
				 *       therefore wait for tx_complete interrupt */
				_timer.usleep(1000);
			}

			_reset_descriptor(_head_index(), packet_phys);
			_head().status |=  Status::Length::bits(p.size());

			/* unset the used bit */
			_head().status &=  Status::Used::clear_mask();

			_advance_head();
		}
};

#endif /* _INCLUDE__DRIVERS__NIC__CADENCE_GEM__TX_BUFFER_DESCRIPTOR_H_ */
