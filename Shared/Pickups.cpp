#include "Pickups.h"

#include <cmath>
#include <cstdlib>

namespace mo
{
	namespace
	{
		bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

		// 한 줄을 칸으로 자른다. # 뒤는 버린다
		size_t SplitFields(const char* begin, const char* end, std::string* fields, size_t maxFields)
		{
			size_t count = 0;
			const char* p = begin;
			while (p < end)
			{
				while (p < end && IsSpace(*p)) ++p;
				if (p >= end || *p == '#') break;
				const char* start = p;
				while (p < end && !IsSpace(*p) && *p != '#') ++p;
				if (count == maxFields) return maxFields + 1;	// 칸이 넘친다
				fields[count++].assign(start, p);
			}
			return count;
		}

		bool ToNumber(const std::string& s, double& out)
		{
			char* end = nullptr;
			out = std::strtod(s.c_str(), &end);
			return end && end != s.c_str() && *end == '\0' && std::isfinite(out);
		}

		bool ToKind(const std::string& s, EPadKind& out)
		{
			for (EPadKind kind : { EPadKind::Heal, EPadKind::Pistol, EPadKind::Rifle, EPadKind::Shotgun })
			{
				if (s == PadKindName(kind))
				{
					out = kind;
					return true;
				}
			}
			return false;
		}
	}

	EPadsLoad ParsePickupPads(const char* text, size_t size, std::vector<PickupPad>& out, size_t& outBadLine)
	{
		constexpr size_t kFields = 11;
		std::vector<PickupPad> pads;
		outBadLine = 0;
		size_t lineNo = 0;
		const char* p = text;
		const char* const end = text + size;
		while (p < end)
		{
			const char* lineEnd = p;
			while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
			++lineNo;
			std::string f[kFields];
			const size_t n = SplitFields(p, lineEnd, f, kFields);
			p = lineEnd < end ? lineEnd + 1 : end;
			if (n == 0) continue;

			PickupPad pad;
			double v[8] = {};
			bool ok = n == kFields && f[0] == "PAD" && ToKind(f[1], pad.kind) && !f[2].empty();
			for (size_t i = 0; ok && i < 8; ++i) ok = ToNumber(f[3 + i], v[i]);
			if (ok)
			{
				pad.name = f[2];
				pad.location = Vec3(v[0], v[1], v[2]);
				pad.radius = v[3];
				pad.halfHeight = v[4];
				pad.heal = v[5];
				pad.respawnSeconds = v[6];
				pad.recheckSeconds = v[7];
				const bool healOk = pad.kind == EPadKind::Heal ? pad.heal > 0 : pad.heal == 0;
				ok = pad.radius > 0 && pad.halfHeight > 0 && healOk && pad.respawnSeconds > 0 && pad.recheckSeconds >= 0;
			}
			if (!ok)
			{
				outBadLine = lineNo;
				return EPadsLoad::BadLine;
			}
			if (pads.size() == kMaxPickupPads)
			{
				outBadLine = lineNo;
				return EPadsLoad::TooMany;
			}
			pads.push_back(pad);
		}
		if (pads.empty()) return EPadsLoad::NoPads;
		out.swap(pads);
		return EPadsLoad::Ok;
	}

	const char* PadsLoadName(EPadsLoad result)
	{
		switch (result)
		{
		case EPadsLoad::Ok: return "ok";
		case EPadsLoad::BadLine: return "bad line";
		case EPadsLoad::NoPads: return "no PAD line";
		case EPadsLoad::TooMany: return "too many pads";
		}
		return "?";
	}

	const char* PadKindName(EPadKind kind)
	{
		switch (kind)
		{
		case EPadKind::Heal: return "heal";
		case EPadKind::Pistol: return "pistol";
		case EPadKind::Rifle: return "rifle";
		case EPadKind::Shotgun: return "shotgun";
		}
		return "?";
	}
}
