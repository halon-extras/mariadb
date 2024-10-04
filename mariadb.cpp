#include <HalonMTA.h>
#include <mysql.h>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <string>
#include <cstring>
#include <syslog.h>

class MariaDBProfile
{
	public:
		std::mutex poolMutex;
		std::condition_variable poolCV;
		std::queue<MYSQL*> poolList;
		std::string cnf;
		size_t pool_size = 1;
		unsigned int read_timeout = 0;
		unsigned int write_timeout = 0;
};

std::map<std::string, MariaDBProfile*> profiles;
MariaDBProfile* default_profile = nullptr;

HALON_EXPORT
int Halon_version()
{
	return HALONMTA_PLUGIN_VERSION;
}

MYSQL* mysql_init_2(MariaDBProfile* profile)
{
	MYSQL* mysql = mysql_init(nullptr);
	mysql_optionsv(mysql, MYSQL_OPT_RECONNECT, (void *)"1");
	mysql_optionsv(mysql, MYSQL_READ_DEFAULT_FILE, (void *)profile->cnf.c_str());
	if (profile->read_timeout)
		mysql_optionsv(mysql, MYSQL_OPT_READ_TIMEOUT, (void *)&profile->read_timeout);
	if (profile->write_timeout)
		mysql_optionsv(mysql, MYSQL_OPT_WRITE_TIMEOUT, (void *)&profile->write_timeout);
	return mysql;
}

HALON_EXPORT
bool Halon_init(HalonInitContext* hic)
{
	mysql_thread_init();

	HalonConfig *cfg;
	HalonMTA_init_getinfo(hic, HALONMTA_INIT_CONFIG, nullptr, 0, &cfg, nullptr);

	auto parseToProfile = [] (HalonConfig* cfg) -> std::pair<bool, MariaDBProfile*> {
		const char* cnf_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "cnf"), nullptr);
		if (!cnf_)
		{
			syslog(LOG_CRIT, "MariaDB plugin has not .cnf file configured");
			return { false , nullptr };
		}

		MariaDBProfile* profile = new MariaDBProfile;
		profile->cnf = cnf_;

		const char* pool_size_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "pool_size"), nullptr);
		if (pool_size_)
			profile->pool_size = strtoul(pool_size_, nullptr, 10);

		const char* read_timeout_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "read_timeout"), nullptr);
		if (read_timeout_)
			profile->read_timeout = (unsigned int)strtoul(read_timeout_, nullptr, 10);

		const char* write_timeout_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "write_timeout"), nullptr);
		if (write_timeout_)
			profile->write_timeout = (unsigned int)strtoul(write_timeout_, nullptr, 10);

		for (size_t i = 0; i < profile->pool_size; ++i)
		{
			MYSQL* mysql = mysql_init_2(profile);
			if (!mysql_real_connect(mysql, nullptr, nullptr, nullptr, nullptr, 0, 0, 0))
				syslog(LOG_ERR, "MariaDB client (%zu/%zu): %s",
					i + 1, profile->pool_size, mysql_error(mysql));
			profile->poolList.push(mysql);
		}
		return { true , profile };
	};

	HalonConfig *hcProfiles = HalonMTA_config_object_get(cfg, "profiles");
	if (hcProfiles)
	{
		const char* default_profile_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "default_profile"), nullptr);

		size_t y = 0;
		HalonConfig* cfgp;
		while ((cfgp = HalonMTA_config_array_get(hcProfiles, y++)))
		{
			std::string type;
			const char* id = HalonMTA_config_string_get(HalonMTA_config_object_get(cfgp, "id"), nullptr);
			if (!id)
				return false;
			auto profile = parseToProfile(cfgp);
			if (!profile.first)
				return false;
			profiles[id] = profile.second;
			if (default_profile_ && strcmp(default_profile_, id) == 0)
				default_profile = profile.second;
		}
	}
	else
	{
		auto profile = parseToProfile(cfg);
		if (!profile.first)
			return false;
		profiles["__default"] = profile.second;
		default_profile = profile.second;
	}

	return true;
}

MYSQL* MySQL_pool_aquire(MariaDBProfile* profile)
{
	std::unique_lock<std::mutex> ul(profile->poolMutex);
	profile->poolCV.wait(ul, [&]() { return !profile->poolList.empty(); });
	MYSQL* mysql = profile->poolList.front();
	profile->poolList.pop();
	ul.unlock();
	if (!mysql_get_host_info(mysql))
	{
		mysql_close(mysql);
		mysql = mysql_init_2(profile);
		if (!mysql_real_connect(mysql, nullptr, nullptr, nullptr, nullptr, 0, 0, 0))
			syslog(LOG_ERR, "MariaDB client: %s", mysql_error(mysql));
	}
	return mysql;
}

void MySQL_pool_release(MariaDBProfile* profile, MYSQL* mysql)
{
	std::unique_lock<std::mutex> ul(profile->poolMutex);
	profile->poolList.push(mysql);
	ul.unlock();
	profile->poolCV.notify_one();
}

HALON_EXPORT
void Halon_cleanup()
{
	for (auto & profile : profiles)
	{
		while (!profile.second->poolList.empty())
		{
			MYSQL* mysql = profile.second->poolList.front();
			mysql_close(mysql);
			profile.second->poolList.pop();
		}
		delete profile.second;
	}
	mysql_thread_end();
}

void buildErrorResponse(MYSQL* mysql, HalonHSLValue* ret)
{
	HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_ARRAY, nullptr, 0);
	HalonHSLValue *key, *val;

	HalonMTA_hsl_value_array_add(ret, &key, &val);
	HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, "errno", 0);
	double err = (double)mysql_errno(mysql);
	HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_NUMBER, &err, 0);

	HalonMTA_hsl_value_array_add(ret, &key, &val);
	HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, "error", 0);
	HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_STRING, mysql_error(mysql), 0);

	HalonMTA_hsl_value_array_add(ret, &key, &val);
	HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, "sqlstate", 0);
	HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_STRING, mysql_sqlstate(mysql), 0);
}

void halon_mysql_query_profile(MariaDBProfile* profile, HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	HalonHSLValue* x = HalonMTA_hsl_argument_get(args, 0);
	char* query = nullptr;
	size_t querylen;
	if (!x || HalonMTA_hsl_value_type(x) != HALONMTA_HSL_TYPE_STRING ||
			!HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &query, &querylen))
	{
		return;
	}

	MYSQL* mysql = MySQL_pool_aquire(profile);

	int r = mysql_real_query(mysql, query, querylen);
	if (r != 0)
	{
		if (mariadb_reconnect(mysql) != 0)
		{
			buildErrorResponse(mysql, ret);
			MySQL_pool_release(profile, mysql);
			return;
		}
		r = mysql_real_query(mysql, query, querylen);
		if (r != 0)
		{
			buildErrorResponse(mysql, ret);
			MySQL_pool_release(profile, mysql);
			return;
		}
	}

	HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_ARRAY, nullptr, 0);
	HalonHSLValue *key, *val, *result_array;

	HalonMTA_hsl_value_array_add(ret, &key, &result_array);
	HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, "result", 0);
	HalonMTA_hsl_value_set(result_array, HALONMTA_HSL_TYPE_ARRAY, nullptr, 0);

	unsigned int fields = mysql_field_count(mysql);
	if (fields != 0)
	{
		MYSQL_RES* result = mysql_store_result(mysql);
		if (!result)
		{
			buildErrorResponse(mysql, ret);
			MySQL_pool_release(profile, mysql);
			return;
		}

		std::vector<std::string> columns;
		for (unsigned int i = 0; i < fields; ++i)
		{
			MYSQL_FIELD* field = mysql_fetch_field_direct(result, i);
			columns.push_back(std::string(field->name, field->name_length));
		}

		MYSQL_ROW row;
		double rc = 0;
		while ((row = mysql_fetch_row(result)))
		{
			unsigned long *lengths = mysql_fetch_lengths(result);

			HalonHSLValue *result_row;
			HalonMTA_hsl_value_array_add(result_array, &key, &result_row);
			HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_NUMBER, &rc, 0);
			for (unsigned int i = 0; i < fields; ++i)
			{
				HalonMTA_hsl_value_array_add(result_row, &key, &val);
				HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, columns[i].c_str(), columns[i].size());
				if (!row[i])
					HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_NONE, nullptr, 0);
				else
					HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_STRING, row[i], lengths[i]);
			}
			++rc;
		}

		mysql_free_result(result);
	}

	HalonMTA_hsl_value_array_add(ret, &key, &val);
	HalonMTA_hsl_value_set(key, HALONMTA_HSL_TYPE_STRING, "affected", 0);
	double affected = (double)mysql_affected_rows(mysql);
	HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_NUMBER, &affected, 0);

	MySQL_pool_release(profile, mysql);
}

HALON_EXPORT
void halon_mysql_query(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	if (!default_profile)
	{
		HalonHSLValue* c = HalonMTA_hsl_throw(hhc);
		HalonMTA_hsl_value_set(c, HALONMTA_HSL_TYPE_EXCEPTION, "no default profile", 0);
		return;
	}
	halon_mysql_query_profile(default_profile, hhc, args, ret);
}

HALON_EXPORT
void halon_mysql_escape_string(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	HalonHSLValue* x = HalonMTA_hsl_argument_get(args, 0);
	char* param = nullptr;
	size_t paramlen;
	if (!x || HalonMTA_hsl_value_type(x) != HALONMTA_HSL_TYPE_STRING ||
			!HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &param, &paramlen))
	{
		return;
	}

	static std::mutex mutex;
	mutex.lock();
	static MYSQL* mysql = nullptr;
	if (!mysql)
		mysql = mysql_init(nullptr);
	char* to = (char*)malloc((paramlen * 2) + 1);
	unsigned long tolen = mysql_real_escape_string(mysql, to, param, paramlen);
	HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_STRING, to, tolen);
	free(to);
	mutex.unlock();
}

void MySQL_query(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	MariaDBProfile* profile = (MariaDBProfile*)HalonMTA_hsl_object_ptr_get(hhc);
	halon_mysql_query_profile(profile, hhc, args, ret);
}

void MySQL(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	MariaDBProfile* profile = default_profile;
	HalonHSLValue* b = HalonMTA_hsl_argument_get(args, 0);
	if (b)
	{
		if (HalonMTA_hsl_value_type(b) != HALONMTA_HSL_TYPE_STRING)
		{
			HalonHSLValue* c = HalonMTA_hsl_throw(hhc);
			HalonMTA_hsl_value_set(c, HALONMTA_HSL_TYPE_EXCEPTION, "argument is not a string", 0);
			return;
		}
		size_t al;
		char* a = nullptr;
		HalonMTA_hsl_value_get(b, HALONMTA_HSL_TYPE_STRING, &a, &al);
		auto pIter = profiles.find(std::string(a, al));
		profile = pIter != profiles.end() ? pIter->second : nullptr;
	}
	if (!profile)
	{
		HalonHSLValue* c = HalonMTA_hsl_throw(hhc);
		HalonMTA_hsl_value_set(c, HALONMTA_HSL_TYPE_EXCEPTION, "unknown MySQL profile", 0);
		return;
	}

	HalonHSLObject* object = HalonMTA_hsl_object_new();
	HalonMTA_hsl_object_type_set(object, "MySQL");
	HalonMTA_hsl_object_register_function(object, "query", MySQL_query);
	HalonMTA_hsl_object_ptr_set(object, profile, nullptr);
	HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_OBJECT, object, 0);
	HalonMTA_hsl_object_delete(object);
}

HALON_EXPORT
bool Halon_hsl_register(HalonHSLRegisterContext* hhrc)
{
	HalonMTA_hsl_register_function(hhrc, "mysql_query", &halon_mysql_query);
	HalonMTA_hsl_register_function(hhrc, "mysql_escape_string", &halon_mysql_escape_string);
	HalonMTA_hsl_module_register_function(hhrc, "mysql_query", &halon_mysql_query);
	HalonMTA_hsl_module_register_function(hhrc, "mysql_escape_string", &halon_mysql_escape_string);
	HalonMTA_hsl_module_register_function(hhrc, "MySQL", MySQL);
	HalonMTA_hsl_module_register_static_function(hhrc, "MySQL", "escape_string", halon_mysql_escape_string);
	return true;
}
