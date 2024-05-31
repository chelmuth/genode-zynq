/*
 * \brief  Policy classes for obtaining DMA-capable memory buffers for packets.
 * \author Johannes Schlatow
 * \date   2022-02-08
 *
 * The DMA memory exactly mirrors the packet-buffer dataspace so that we can
 * reuse the packet-buffer management and thus simply calculate the DMA address
 * from a packet descriptor and vice versa.
 *
 * Note on alignment:
 * According to ug585, an alignment to cache line boundaries is beneficial
 * for performance but not mandatory. The packets from the packet allocator
 * actually offsets the packet address by 2-bytes. Since the allocated
 * buffer is actually cache-line aligned and the first two bytes of
 * the allocated buffer remain unused, I assume there is no
 * performance penalty.
 */

/*
 * Copyright (C) 2022 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _DRIVERS__NIC__CADENCE_GEM__DMA_POOL_H_
#define _DRIVERS__NIC__CADENCE_GEM__DMA_POOL_H_

/* Genode includes */
#include <platform_session/connection.h>
#include <os/packet_stream.h>

namespace Cadence_gem
{
	using namespace Genode;
	using Packet_descriptor = Genode::Packet_descriptor;

	class Dma_pool_base;

	template <typename PACKET_STREAM>
	class Buffered_dma_pool;
}


class Cadence_gem::Dma_pool_base
{
	protected:
		addr_t const _dma_base_addr;
		size_t const _size;

	public:
		Dma_pool_base(addr_t dma_base, size_t size)
		: _dma_base_addr(dma_base),
		  _size(size)
		{ }

		/* return dma address for given packet descriptor */
		addr_t dma_addr(Packet_descriptor const &p) { return _dma_base_addr + p.offset(); }

		/* return dma address containing packet content of given packet descriptor */
		addr_t dma_addr_with_content(Packet_descriptor const &p);

		/* return packet descriptor for given dma address */
		Packet_descriptor packet_descriptor(addr_t dma_addr, size_t len)
		{
			if (dma_addr < _dma_base_addr || dma_addr + len > _dma_base_addr + _size)
				return Packet_descriptor(0, 0);

			return Packet_descriptor(dma_addr - _dma_base_addr, len);
		}

		/* return packet descriptor containing content from given dma address */
		Packet_descriptor packet_descriptor_with_content(addr_t dma_addr, size_t len);

};


template <typename PACKET_STREAM>
class Cadence_gem::Buffered_dma_pool : private Platform::Dma_buffer,
                                       public  Dma_pool_base
{
	private:
		PACKET_STREAM &_packet_stream;

		void* _local_packet_addr(Packet_descriptor const &p) {
			return reinterpret_cast<void*>(Dma_buffer::local_addr<uint8_t>() + p.offset()); }

	public:
		using Dma_pool_base::dma_addr;

		Packet_descriptor packet_descriptor_with_content(addr_t dma_addr, size_t len)
		{
			/* copy content from DMA memory to packet descriptor */
			Packet_descriptor p = packet_descriptor(dma_addr, len);
			memcpy(_packet_stream.packet_content(p), _local_packet_addr(p), p.size());
			return p;
		}

		addr_t dma_addr_with_content(Packet_descriptor const &p)
		{
			/* copy content from packet descriptor to DMA memory */
			memcpy(_local_packet_addr(p), _packet_stream.packet_content(p), p.size());
			return Dma_pool_base::dma_addr(p);
		}

		Buffered_dma_pool(Platform::Connection &platform, PACKET_STREAM &ps)
		: Dma_buffer(platform, ps.ds_size(), UNCACHED),
		  Dma_pool_base(Dma_buffer::dma_addr(), ps.ds_size()),
		  _packet_stream(ps)
		{
			if (!_dma_base_addr)
				error(__PRETTY_FUNCTION__, ": Could not get DMA address of dataspace");
		}
};

#endif /* _DRIVERS__NIC__CADENCE_GEM__DMA_POOL_H_ */

