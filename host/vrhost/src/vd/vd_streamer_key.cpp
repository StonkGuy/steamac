// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal ECMA-335 (CLI metadata) reader for vd_streamer_key.h: PE sections, CLI header, metadata root, the #~
// table stream (row counts and sizes of tables 0x00..0x1D), #Strings, method bodies. Own implementation from the
// public ECMA-335 specification (Partition II sections 22-25).
#include "vd/vd_streamer_key.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace vrhost::vd
{
namespace
{
struct Fail
{
	std::string why;
};

struct Bytes
{
	const std::vector<uint8_t> & b;
	void need(size_t off, size_t n) const
	{
		if (off > b.size() or n > b.size() - off)
			throw Fail{"truncated file"};
	}
	uint8_t u8(size_t o) const { return need(o, 1), b[o]; }
	uint16_t u16(size_t o) const { return need(o, 2), uint16_t(b[o] | b[o + 1] << 8); }
	uint32_t u32(size_t o) const { return need(o, 4), uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; }
};

enum Table
{
	t_module = 0x00, t_typeref, t_typedef, t_fieldptr, t_field, t_methodptr, t_methoddef, t_paramptr, t_param,
	t_interfaceimpl, t_memberref, t_constant, t_customattribute, t_fieldmarshal, t_declsecurity, t_classlayout,
	t_fieldlayout, t_standalonesig, t_eventmap, t_eventptr, t_event, t_propertymap, t_propertyptr, t_property,
	t_methodsemantics, t_methodimpl, t_moduleref, t_typespec, t_implmap, t_fieldrva, t_enclog, t_encmap, t_assembly,
	t_assemblyprocessor, t_assemblyos, t_assemblyref, t_assemblyrefprocessor, t_assemblyrefos, t_file,
	t_exportedtype, t_manifestresource, t_nestedclass, t_genericparam, t_methodspec, t_genericparamconstraint,
	t_count = 64,
};

struct Metadata
{
	const Bytes & f;
	std::array<uint32_t, t_count> rows{};
	std::array<size_t, t_count> row_size{}, table_off{};
	size_t strings_off = 0, strings_size = 0;
	int str_idx = 2, guid_idx = 2, blob_idx = 2;

	int idx(Table t) const { return rows[t] < 0x10000 ? 2 : 4; }
	int coded(std::initializer_list<Table> tables) const
	{
		int bits = 0;
		while ((size_t(1) << bits) < tables.size())
			++bits;
		uint32_t max = 0;
		for (auto t: tables)
			max = std::max(max, rows[t]);
		return max < (uint32_t(1) << (16 - bits)) ? 2 : 4;
	}
	uint32_t read(size_t off, int size) const { return size == 2 ? f.u16(off) : f.u32(off); }
	std::string string(uint32_t i) const
	{
		if (i >= strings_size)
			throw Fail{"bad string index"};
		size_t o = strings_off + i;
		std::string s;
		while (f.u8(o) != 0)
			s += char(f.u8(o++));
		return s;
	}
};

std::vector<uint8_t> load(const std::filesystem::path & p)
{
	std::ifstream in(p, std::ios::binary);
	if (not in)
		throw Fail{"cannot open " + p.string()};
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (b.size() > (64u << 20))
		throw Fail{"file too large"};
	return b;
}

AesKey read_key(const std::vector<uint8_t> & data, const std::string & type_name)
{
	Bytes f{data};
	// --- PE: sections and the CLI header data directory (index 14) ----------------------------------------------
	if (f.u16(0) != 0x5a4d)
		throw Fail{"not a PE file"};
	size_t pe = f.u32(0x3c);
	if (f.u32(pe) != 0x00004550)
		throw Fail{"no PE signature"};
	uint16_t nsect = f.u16(pe + 6), opt_size = f.u16(pe + 20);
	size_t opt = pe + 24;
	uint16_t magic = f.u16(opt);
	size_t dirs = opt + (magic == 0x20b ? 112 : magic == 0x10b ? 96 : throw Fail{"unknown optional header"});
	uint32_t cli_rva = f.u32(dirs + 14 * 8);
	if (cli_rva == 0)
		throw Fail{"not a .NET assembly"};
	size_t sect = opt + opt_size;
	auto offset = [&](uint32_t rva) -> size_t {
		for (uint16_t i = 0; i < nsect; ++i)
		{
			size_t s = sect + i * 40;
			uint32_t va = f.u32(s + 12), vsize = std::max(f.u32(s + 8), f.u32(s + 16)), raw = f.u32(s + 20);
			if (rva >= va and rva < va + vsize)
				return raw + (rva - va);
		}
		throw Fail{"RVA outside the sections"};
	};
	size_t cli = offset(cli_rva);
	size_t md = offset(f.u32(cli + 8));

	// --- metadata root and streams -------------------------------------------------------------------------------
	if (f.u32(md) != 0x424a5342) // "BSJB"
		throw Fail{"no metadata root"};
	uint32_t vlen = f.u32(md + 12);
	size_t p = md + 16 + vlen + 2;
	uint16_t nstreams = f.u16(p);
	p += 2;
	size_t tables = 0;
	Metadata m{f};
	for (uint16_t i = 0; i < nstreams; ++i)
	{
		uint32_t off = f.u32(p), size = f.u32(p + 4);
		std::string name;
		size_t n = p + 8;
		while (f.u8(n))
			name += char(f.u8(n++));
		p = (n + 4) & ~size_t(3);
		if (name == "#~")
			tables = md + off;
		else if (name == "#-")
			throw Fail{"uncompressed metadata tables are not supported"};
		else if (name == "#Strings")
			m.strings_off = md + off, m.strings_size = size;
	}
	if (not tables or not m.strings_off)
		throw Fail{"metadata streams missing"};

	// --- #~: heap sizes, present tables, row counts ---------------------------------------------------------------
	uint8_t heaps = f.u8(tables + 6);
	m.str_idx = heaps & 1 ? 4 : 2;
	m.guid_idx = heaps & 2 ? 4 : 2;
	m.blob_idx = heaps & 4 ? 4 : 2;
	uint64_t valid = uint64_t(f.u32(tables + 8)) | uint64_t(f.u32(tables + 12)) << 32;
	size_t q = tables + 24;
	for (int t = 0; t < t_count; ++t)
		if (valid >> t & 1)
		{
			m.rows[t] = f.u32(q);
			q += 4;
		}
	if (m.rows[t_fieldptr] or m.rows[t_methodptr])
		throw Fail{"indirection tables are not supported"};
	const int S = m.str_idx, G = m.guid_idx, B = m.blob_idx;
	const int typedef_or_ref = m.coded({t_typedef, t_typeref, t_typespec});
	const int has_constant = m.coded({t_field, t_param, t_property});
	const int has_ca = m.coded({t_methoddef, t_field, t_typeref, t_typedef, t_param, t_interfaceimpl, t_memberref, t_module,
	                            t_declsecurity, t_property, t_event, t_standalonesig, t_moduleref, t_typespec, t_assembly,
	                            t_assemblyref, t_file, t_exportedtype, t_manifestresource, t_genericparam,
	                            t_genericparamconstraint, t_methodspec});
	const int has_marshal = m.coded({t_field, t_param});
	const int has_decl = m.coded({t_typedef, t_methoddef, t_assembly});
	const int mr_parent = m.coded({t_typedef, t_typeref, t_moduleref, t_methoddef, t_typespec});
	const int has_sem = m.coded({t_event, t_property});
	const int mdef_or_ref = m.coded({t_methoddef, t_memberref});
	const int forwarded = m.coded({t_field, t_methoddef});
	const int ca_type = m.coded({t_methoddef, t_memberref, t_methoddef, t_memberref, t_methoddef}); // 3 bits
	const int res_scope = m.coded({t_module, t_moduleref, t_assemblyref, t_typeref});
	m.row_size[t_module] = 2 + S + 3 * G;
	m.row_size[t_typeref] = res_scope + 2 * S;
	m.row_size[t_typedef] = 4 + 2 * S + typedef_or_ref + m.idx(t_field) + m.idx(t_methoddef);
	m.row_size[t_field] = 2 + S + B;
	m.row_size[t_methoddef] = 4 + 2 + 2 + S + B + m.idx(t_param);
	m.row_size[t_param] = 2 + 2 + S;
	m.row_size[t_interfaceimpl] = m.idx(t_typedef) + typedef_or_ref;
	m.row_size[t_memberref] = mr_parent + S + B;
	m.row_size[t_constant] = 2 + has_constant + B;
	m.row_size[t_customattribute] = has_ca + ca_type + B;
	m.row_size[t_fieldmarshal] = has_marshal + B;
	m.row_size[t_declsecurity] = 2 + has_decl + B;
	m.row_size[t_classlayout] = 2 + 4 + m.idx(t_typedef);
	m.row_size[t_fieldlayout] = 4 + m.idx(t_field);
	m.row_size[t_standalonesig] = B;
	m.row_size[t_eventmap] = m.idx(t_typedef) + m.idx(t_event);
	m.row_size[t_eventptr] = m.idx(t_event);
	m.row_size[t_event] = 2 + S + typedef_or_ref;
	m.row_size[t_propertymap] = m.idx(t_typedef) + m.idx(t_property);
	m.row_size[t_propertyptr] = m.idx(t_property);
	m.row_size[t_property] = 2 + S + B;
	m.row_size[t_methodsemantics] = 2 + m.idx(t_methoddef) + has_sem;
	m.row_size[t_methodimpl] = m.idx(t_typedef) + 2 * mdef_or_ref;
	m.row_size[t_moduleref] = S;
	m.row_size[t_typespec] = B;
	m.row_size[t_implmap] = 2 + forwarded + S + m.idx(t_moduleref);
	m.row_size[t_fieldrva] = 4 + m.idx(t_field);
	size_t off = q;
	for (int t = 0; t <= t_fieldrva; ++t)
	{
		if (m.rows[t] and not m.row_size[t])
			throw Fail{"unsupported metadata table " + std::to_string(t)};
		m.table_off[t] = off;
		off += size_t(m.rows[t]) * m.row_size[t];
	}
	auto row = [&](Table t, uint32_t r) { return m.table_off[t] + size_t(r - 1) * m.row_size[t]; }; // 1-based

	// --- the class, its .cctor ------------------------------------------------------------------------------------
	uint32_t first_method = 0, end_method = m.rows[t_methoddef] + 1;
	size_t td_methods = 4 + 2 * S + typedef_or_ref + m.idx(t_field);
	for (uint32_t r = 1; r <= m.rows[t_typedef]; ++r)
		if (m.string(m.read(row(t_typedef, r) + 4, S)) == type_name)
		{
			first_method = m.read(row(t_typedef, r) + td_methods, m.idx(t_methoddef));
			if (r < m.rows[t_typedef])
				end_method = m.read(row(t_typedef, r + 1) + td_methods, m.idx(t_methoddef));
			break;
		}
	if (not first_method)
		throw Fail{"discovery class not found"};
	uint32_t cctor_rva = 0;
	for (uint32_t r = first_method; r < end_method; ++r)
		if (m.string(m.read(row(t_methoddef, r) + 8, S)) == ".cctor")
			cctor_rva = f.u32(row(t_methoddef, r));
	if (not cctor_rva)
		throw Fail{"static constructor not found"};
	size_t body = offset(cctor_rva);
	size_t code, code_size;
	if ((f.u8(body) & 3) == 2)
		code = body + 1, code_size = f.u8(body) >> 2;
	else if ((f.u8(body) & 3) == 3)
		code = body + 4 * (f.u16(body) >> 12), code_size = f.u32(body + 4);
	else
		throw Fail{"bad method header"};
	f.need(code, code_size);

	// --- ldc.i4.s N; newarr T; dup; ldtoken F (field) -> F's RVA data -----------------------------------------------
	auto array_field = [&](uint8_t n, size_t from, size_t & at) -> uint32_t {
		for (size_t i = from; i + 13 <= code_size; ++i)
		{
			const uint8_t * c = data.data() + code + i;
			if (c[0] == 0x1f and c[1] == n and c[2] == 0x8d and c[7] == 0x25 and c[8] == 0xd0 and c[12] == 0x04)
			{
				at = i + 13;
				return uint32_t(c[9]) | uint32_t(c[10]) << 8 | uint32_t(c[11]) << 16;
			}
		}
		throw Fail{"array initialiser not found"};
	};
	auto rva_bytes = [&](uint32_t field, size_t n, uint8_t * out) {
		for (uint32_t r = 1; r <= m.rows[t_fieldrva]; ++r)
			if (m.read(row(t_fieldrva, r) + 4, m.idx(t_field)) == field)
			{
				size_t o = offset(f.u32(row(t_fieldrva, r)));
				f.need(o, n);
				memcpy(out, data.data() + o, n);
				return;
			}
		throw Fail{"field data not found"};
	};
	size_t at = 0;
	uint32_t key_field = array_field(32, 0, at);
	uint32_t iv_field = array_field(16, at, at);
	AesKey k;
	rva_bytes(key_field, 32, k.key.data());
	rva_bytes(iv_field, 16, k.iv.data());
	return k;
}
} // namespace

StreamerKeyResult read_streamer_key(const std::filesystem::path & assembly, const std::string & type_name)
{
	try
	{
		auto data = load(assembly);
		return {read_key(data, type_name), ""};
	}
	catch (Fail & e)
	{
		return {std::nullopt, e.why};
	}
}

std::vector<std::filesystem::path> streamer_assembly_candidates()
{
	std::vector<std::filesystem::path> roots{"/Applications"};
	if (const char * home = getenv("HOME"))
		roots.push_back(std::filesystem::path(home) / "Applications");
	std::vector<std::filesystem::path> out;
	for (auto & r: roots)
		for (const char * arch: {"osx-arm64", "osx-x64"})
			out.push_back(r / "Virtual Desktop Streamer.app/Contents/MonoBundle/.xamarin" / arch / "VirtualDesktop.Net.dll");
	return out;
}

StreamerKeyResult read_installed_streamer_key()
{
	std::string errors;
	for (auto & p: streamer_assembly_candidates())
	{
		std::error_code ec;
		if (not std::filesystem::exists(p, ec))
			continue;
		auto r = read_streamer_key(p);
		if (r.key)
			return r;
		errors += (errors.empty() ? "" : "; ") + p.string() + ": " + r.error;
	}
	return {std::nullopt, errors.empty() ? "Virtual Desktop Streamer is not installed" : errors};
}
} // namespace vrhost::vd
