#include "inc.hpp"

#define GAME_VERISON 2699
#define XOR_KEY 0xb7ac4b1c

#define PROTECTION_KV(pr) { pr, #pr }
int number=0;
std::unordered_map<uint32_t, std::string_view> protection_type
{
	PROTECTION_KV(PAGE_EXECUTE),
	PROTECTION_KV(PAGE_EXECUTE_READ),
	PROTECTION_KV(PAGE_EXECUTE_READWRITE),
	PROTECTION_KV(PAGE_EXECUTE_WRITECOPY),
	PROTECTION_KV(PAGE_NOACCESS),
	PROTECTION_KV(PAGE_READONLY),
	PROTECTION_KV(PAGE_READWRITE),
	PROTECTION_KV(PAGE_WRITECOPY),
	PROTECTION_KV(PAGE_TARGETS_INVALID),
	PROTECTION_KV(PAGE_TARGETS_NO_UPDATE),
	PROTECTION_KV(PAGE_GUARD),
	PROTECTION_KV(PAGE_NOCACHE),
	PROTECTION_KV(PAGE_WRITECOMBINE)
};

// "a modified JOAAT that is initialized with the CRC-32 polynomial."  - pelecanidae
uint32_t sig_joaat(uint8_t* input, uint32_t size)
{
	uint32_t hash = 0x4c11db7;
	for (uint32_t i = 0; i < size; i++)
	{
		hash += input[i];
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

struct sig
{
	uint32_t m_hash;
	uint8_t m_start_byte;
	uint32_t m_start_page;
	uint32_t m_end_page;
	uint32_t m_protect_flag;
	uint32_t m_size;
	uint32_t m_region_size_estimate;
	uint32_t m_game_version;
	uint32_t m_xor_const;

	sig(std::vector<uint32_t> data)
	{
		m_hash = data[3];                                                   // joaat hash of scan
		m_xor_const = XOR_KEY ^ m_hash;                                     // used for decryption of values
		m_start_byte = (m_xor_const ^ data[2]) >> 24 & 0xff;                // starting byte of scan
		m_start_page = (m_xor_const ^ data[1]) & 0xffff;                    // start page of scan (base_address + 4096 * a1->start_page)
		m_end_page = (m_xor_const ^ data[1]) >> 16 & 0xffff;                // end page of scan (base_address + 4096 * a1->start_page)
		m_protect_flag = (m_xor_const ^ data[4]) >> 8;                      // protection flag of scanned region, usually PAGE_READONLY or PAGE_EXECUTE_READWRITE, if different it won't scan
		m_size = (m_xor_const ^ data[2]) >> 18 & 0x3f;                      // length of scanned string or bytes
		m_region_size_estimate = ((m_xor_const ^ data[2]) & 0x3ffff) << 10; // region_size > m_region_size_estimate * 0.9 && region_size < m_region_size_estimate * 1.1, otherwise scan doesn't run.
		m_game_version = (m_xor_const ^ data[0]) & 0xffff;                  // game version when scan was added, if game version differs scan will not run
	}

	uint8_t* scan(uint8_t* data, size_t size)
	{
		for (auto ptr = data; ptr < data + size - m_size; ptr++)
		{
			if (*ptr != m_start_byte)
				continue;
			if (sig_joaat(ptr, m_size) == m_hash)
				return ptr;
		}
		return 0;
	}
};

bool is_ascii(uint8_t* start, uint32_t size)
{
	return !std::any_of(start, start + size, [](uint8_t c) { return c > 127; });
}

uint32_t safe_get_uint(rapidjson::Value& value)
{
	return value.IsUint() ? value.GetUint() : value.GetInt();
}

rapidjson::Document download_tunables()
{
	cpr::Response r = cpr::Get(cpr::Url{ "http://prod.cloud.rockstargames.com/titles/gta5/pcros/0x1a098062.json" });
	uint8_t key[] = { 0xf0, 0x6f, 0x12, 0xf4, 0x9b, 0x84, 0x3d, 0xad, 0xe4, 0xa7, 0xbe, 0x05, 0x35, 0x05, 0xb1, 0x9c, 0x9e, 0x41, 0x5c, 0x95, 0xd9, 0x37, 0x53, 0x45, 0x0a, 0x26, 0x91, 0x44, 0xd5, 0x9a, 0x01, 0x15 };
	AES aes(AESKeyLength::AES_256);
	auto crypted_chunk = r.text.size() - (r.text.size() % 16);
	auto out = aes.DecryptECB((uint8_t*)r.text.data(), (uint32_t)crypted_chunk, key);
	std::string j((char*)out, crypted_chunk);
	j += std::string(r.text.data() + crypted_chunk, (r.text.size() % 16));
	delete[] out;
	rapidjson::Document d;
	d.Parse(j);
	return d;
}

void loop_bonus(rapidjson::Document& doc, uint8_t* data, size_t size, std::string filename, FILE* stream)
{
	if (!doc.HasMember("bonus"))
		return;
	for (auto& bonus : doc["bonus"].GetArray())
	{
		auto values = bonus.GetArray();
		sig s({ safe_get_uint(values[0]), safe_get_uint(values[1]), safe_get_uint(values[2]), safe_get_uint(values[3]), safe_get_uint(values[4]) });
		/*if (s.m_game_version != GAME_VERISON)
			continue;*/
		if (auto location = s.scan(data, size))
		{
			if (is_ascii(location, s.m_size))
			{
				printf("(%s) \"%.*s\" (%u) (v%d) (%s) (~%.3f kb region)\n", filename.c_str(), s.m_size, location, s.m_size, s.m_game_version, protection_type.contains(s.m_protect_flag) ? protection_type[s.m_protect_flag].data() : ("PAGE_UNK" + std::to_string(s.m_protect_flag)).c_str(), s.m_region_size_estimate / 1000.0);
				fprintf(stream, "(%s) \"%.*s\" (%u) (v%d) (%s) (~%.3f kb region)\n", filename.c_str(), s.m_size, location, s.m_size, s.m_game_version, protection_type.contains(s.m_protect_flag) ? protection_type[s.m_protect_flag].data() : ("PAGE_UNK" + std::to_string(s.m_protect_flag)).c_str(), s.m_region_size_estimate / 1000.0);
				number++;
			}
			else
			{
				printf("(%s) { ", filename.c_str());
				fprintf(stream, "(%s) { ", filename.c_str());
				for (auto i = 0ull; i < s.m_size; i++)
				{
					printf("%02hhx ", location[i]);
					fprintf(stream, "%02hhx ", location[i]);
				}
				printf("} (%u) (v%d) (%s) (~%.3f kb region)\n", s.m_size, s.m_game_version, protection_type.contains(s.m_protect_flag) ? protection_type[s.m_protect_flag].data() : ("PAGE_UNK" + std::to_string(s.m_protect_flag)).c_str(), s.m_region_size_estimate / 1000.0);
				fprintf(stream, "} (%u) (v%d) (%s) (~%.3f kb region)\n", s.m_size, s.m_game_version, protection_type.contains(s.m_protect_flag) ? protection_type[s.m_protect_flag].data() : ("PAGE_UNK" + std::to_string(s.m_protect_flag)).c_str(), s.m_region_size_estimate / 1000.0);
				number++;
			}
		}
	}
}

// Used to verify the integrity of the bonus struct once loaded in game.
std::array<uint8_t, 20> get_bonus_sha(rapidjson::Document& doc)
{
	sha1::SHA1 s;
	uint32_t sig_count = 0;
	auto sha_update = [&](uint32_t data) { s.processBytes(&data, 4); };
	sha_update(GAME_VERISON);
	for (auto& bonus : doc["bonus"].GetArray())
	{
		auto values = bonus.GetArray();
		sig s({ safe_get_uint(values[0]), safe_get_uint(values[1]), safe_get_uint(values[2]), safe_get_uint(values[3]), safe_get_uint(values[4]) });
		if (s.m_game_version == GAME_VERISON)
		{
			sha_update(safe_get_uint(values[0]));
			sha_update(safe_get_uint(values[1]));
			sha_update(safe_get_uint(values[2]));
			sha_update(safe_get_uint(values[3]));
			sha_update((safe_get_uint(values[4]) ^ XOR_KEY ^ safe_get_uint(values[3])) & 0xFFFFFF);
			sig_count++;
		}
	}
	sha_update(sig_count);
	std::array<uint8_t, 20> hash;
	s.getDigestBytes(hash.data());
}


int main()
{
	FILE* fp; errno_t err;
	printf("��ʼ����������....\n");
	auto tunables = download_tunables();
	printf("�������������\n");
	if (!std::filesystem::exists("files"))
	{
		if (!std::filesystem::is_directory(".//files"))
		{
			std::filesystem::create_directory(".//files");
		}
	}
	err = fopen_s(&fp, "data.txt", "w");
	if (err == 0)
	{
		printf("����ɨ��ƥ��������....\n");
		for (const auto& entry : std::filesystem::recursive_directory_iterator("./files/"))
		{
			std::ifstream i(entry.path(), std::ios::binary);
			std::vector<uint8_t> contents((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
			loop_bonus(tunables, contents.data(), contents.size(), entry.path().filename().string(), fp);
		}
		if (fp)
		{
			fclose(fp);
		}
		printf("ƥ�����\n");
		number != 0 ? printf("��ƥ�䵽������%d��\n", number) : printf("û��ƥ�䵽�κ�������\n");
	}
	system("pause");
	return 0;
}