/*
 * \brief  Bitstream reader for Xilinx FPGAs
 * \author Johannes Schlatow
 * \date   2021-11-24
 *
 * The bitstream file may come as a .bit or a .bin file. The .bin file
 * contains the raw data that is to be transferred to the FPGA via the PCAP
 * interface. The .bit file comprises an extra header of variable length
 * followed by the raw data stream in swapped byte order.
 *
 * For ease of use, we detect whether there is header information and
 * perform the byte swapping.
 */

/*
 * Copyright (C) 2021 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _DRIVERS__FPGA__BITSTREAM_H_
#define _DRIVERS__FPGA__BITSTREAM_H_

#include <base/attached_rom_dataspace.h>
#include <util/endian.h>

namespace Fpga {
	using namespace Genode;

	class Bitstream;
}

class Fpga::Bitstream
{
	public:
		struct Format_error : Genode::Exception { };

	private:
		struct Header_length_error : Exception { };
		struct Header_error        : Exception { };

		enum Format { INVALID     = 0,
		              RAW         = 1,
		              SWAP_NEEDED = 2 };

		size_t                  _bitstream_size { 0 };
		Format                  _format { INVALID };
		size_t                  _offset { 0 };
		Attached_rom_dataspace &_rom;

		static Format _detect_format(char * buf, size_t buf_sz, size_t & offset, size_t & length);
		static size_t _parse_header_field(char magic, char * buf, size_t buf_sz, size_t pos);
		static size_t _parse_size_field(char * buf, size_t buf_sz, size_t pos, size_t & size);

		size_t _read_swapped(char * dst, size_t size) const;

		char *_bitstream_start() const { return _rom.local_addr<char>() + _offset; }

	public:

		Bitstream(Attached_rom_dataspace &rom, size_t max_size = 0)
		: _rom(rom)
		{
			size_t length { 0 };

			/* detect format and store in members */
			_format = _detect_format(_rom.local_addr<char>(), _rom.size(), _offset, length);
			switch (_format)
			{
				case INVALID:
					error("Invalid bitstream file");
					throw Format_error();
				case RAW:
					if (max_size == 0) {
						warning("no max_size attribute provided for bitstream in raw/bin format");
						_bitstream_size = _rom.size();
					}
					else
						_bitstream_size = min(_rom.size(), max_size);
					break;
				case SWAP_NEEDED:
					_bitstream_size = length;
					break;
			}
		}

		size_t read_bitstream(char *dst, size_t dst_sz) const
		{
			size_t sz = min(_bitstream_size, dst_sz);

			switch (_format)
			{
				case RAW:
					Genode::memcpy(dst, _bitstream_start(), sz);
					return sz;
				case SWAP_NEEDED:
					return _read_swapped(dst, sz);
				default:
					return 0;
			}
		}

		size_t size() const { return _bitstream_size; }
};


Genode::size_t
Fpga::Bitstream::_parse_header_field(char            magic,
                                     char          * buf,
                                     Genode::size_t  buf_sz,
                                     Genode::size_t  pos)
{
	if (pos+3 >= buf_sz)
		throw Header_length_error();

	if (buf[pos] != magic) {
		throw Header_error();
	}

	uint16_t length = host_to_big_endian(*reinterpret_cast<uint16_t*>(&buf[pos+1]));
	return length+3;
}


Genode::size_t
Fpga::Bitstream::_parse_size_field(char           * buf,
                                   Genode::size_t   buf_sz,
                                   Genode::size_t   pos,
                                   Genode::size_t & size)
{
	if (pos+5 >= buf_sz)
		throw Header_length_error();

	if (buf[pos] != 0x65)
		throw Header_error();

	size = host_to_big_endian(*reinterpret_cast<uint32_t*>(&buf[pos+1]));
	return 5;
}


Fpga::Bitstream::Format
Fpga::Bitstream::_detect_format(char           * buf,
                                Genode::size_t   buf_sz,
                                Genode::size_t & offset,
                                Genode::size_t & length)
{
	enum { RAW_START     = 0xffffffff };
	enum { HDR_START     = 0xf00f0900 };
	enum { MAGIC_SWAPPED = 0x665599aa };
	enum { MAGIC         = 0xaa995566 };

	uint32_t first_word = *reinterpret_cast<uint32_t*>(buf);

	if (first_word == HDR_START)
	{
		/**
		 * find length field in header
		 * see http://www.fpga-faq.com/FAQ_Pages/0026_Tell_me_about_bit_files.htm
		 *
		 * the first two bytes (0x0009) specify a header field 0 of 9 bytes length.
		 * this is followed by another 2 byte length field,
		 * thus we skip the first 13 bytes
		 */
		size_t byte = 13;
		try {
			byte += _parse_header_field(0x61, buf, buf_sz, byte);
			byte += _parse_header_field(0x62, buf, buf_sz, byte);
			byte += _parse_header_field(0x63, buf, buf_sz, byte);
			byte += _parse_header_field(0x64, buf, buf_sz, byte);
			byte += _parse_size_field(buf, buf_sz, byte, length);
		} catch (...) { return INVALID; }

		offset = byte;
	}
	else if (first_word == RAW_START)
	{
		offset = 0;
	}
	else {
		return INVALID;
	}

	/* find MAGIC or MAGIC_SWAPPED */
	for (size_t byte = offset; byte < buf_sz; byte++) {
		uint32_t cur_word = *reinterpret_cast<uint32_t*>(&buf[byte]);
		if (cur_word == MAGIC)
			return RAW;
		if (cur_word == MAGIC_SWAPPED)
			return SWAP_NEEDED;
	}

	return INVALID;
}


Genode::size_t Fpga::Bitstream::_read_swapped(char * dst, size_t size) const
{
	size_t written { 0 };

	if (size & 0x3)
		error("Skipping last incomplete word of bitstream");

	uint32_t * dst_words = reinterpret_cast<uint32_t*>(dst);
	uint32_t * src_words = reinterpret_cast<uint32_t*>(_bitstream_start());
	for (size_t word { 0 }; word < size / 4; word++, written += 4)
	{
		/* copy word and swap endianess */
		dst_words[word] = host_to_big_endian(src_words[word]);
	}

	return written;
}

#endif /* _DRIVERS__FPGA__BITSTREAM_H_ */
