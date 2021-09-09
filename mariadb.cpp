#include <HalonMTA.h>
#include <mysql.h>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <syslog.h>

std::mutex poolMutex;
std::condition_variable poolCV;
std::queue<MYSQL*> poolList;

HALON_EXPORT
int Halon_version()
{
	return HALONMTA_PLUGIN_VERSION;
}

HALON_EXPORT
bool Halon_init(HalonInitContext* hic)
{
	HalonConfig *cfg;
	HalonMTA_init_getinfo(hic, HALONMTA_INIT_CONFIG, nullptr, 0, &cfg, nullptr);
	const char* cnf = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "cnf"), nullptr);
	if (!cnf)
	{
		syslog(LOG_CRIT, "MariaDB plugin has not .cnf file configured");
		return false;
	}

	size_t pool_size = 1;
	const char* pool_size_ = HalonMTA_config_string_get(HalonMTA_config_object_get(cfg, "pool_size"), nullptr);
	if (pool_size_)
		pool_size = strtoul(pool_size_, nullptr, 10);

	mysql_thread_init();

	for (size_t i = 0; i < pool_size; ++i)
	{
		MYSQL* mysql = mysql_init(nullptr);
		mysql_optionsv(mysql, MYSQL_OPT_RECONNECT, (void *)"1");
		mysql_optionsv(mysql, MYSQL_READ_DEFAULT_FILE, (void *)cnf);
		if (!mysql_real_connect(mysql, nullptr, nullptr, nullptr, nullptr, 0, 0, 0))
			syslog(LOG_ERR, "MariaDB client (%zu/%zu): %s",
				i + 1, pool_size, mysql_error(mysql));
		poolList.push(mysql);
	}
	return true;
}

MYSQL* MySQL_pool_aquire()
{
	std::unique_lock<std::mutex> ul(poolMutex);
	poolCV.wait(ul, [&]() { return !poolList.empty(); });
	MYSQL* mysql = poolList.front();
	poolList.pop();
	ul.unlock();
	return mysql;
}

void MySQL_pool_release(MYSQL* mysql)
{
	std::unique_lock<std::mutex> ul(poolMutex);
	poolList.push(mysql);
	ul.unlock();
	poolCV.notify_one();
}

HALON_EXPORT
void Halon_cleanup()
{
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

HALON_EXPORT
void halon_mysql_query(HalonHSLContext* hhc, HalonHSLArguments* args, HalonHSLValue* ret)
{
	HalonHSLValue* x = HalonMTA_hsl_argument_get(args, 0);
	char* query = nullptr;
	size_t querylen;
	if (!x || HalonMTA_hsl_value_type(x) != HALONMTA_HSL_TYPE_STRING ||
			!HalonMTA_hsl_value_get(x, HALONMTA_HSL_TYPE_STRING, &query, &querylen))
	{
		return;
	}

	MYSQL* mysql = MySQL_pool_aquire();

	int r = mysql_real_query(mysql, query, querylen);
	if (r != 0)
	{
		if (mariadb_reconnect(mysql) != 0)
		{
			buildErrorResponse(mysql, ret);
			MySQL_pool_release(mysql);
			return;
		}
		r = mysql_real_query(mysql, query, querylen);
		if (r != 0)
		{
			buildErrorResponse(mysql, ret);
			MySQL_pool_release(mysql);
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
			MySQL_pool_release(mysql);
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
	double affected = mysql_affected_rows(mysql);
	HalonMTA_hsl_value_set(val, HALONMTA_HSL_TYPE_NUMBER, &affected, 0);

	MySQL_pool_release(mysql);
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

	MYSQL* mysql = MySQL_pool_aquire();
	char* to = (char*)malloc((paramlen * 2) + 1);
	unsigned long tolen = mysql_real_escape_string(mysql, to, param, paramlen);
	HalonMTA_hsl_value_set(ret, HALONMTA_HSL_TYPE_STRING, to, tolen);
	free(to);
	MySQL_pool_release(mysql);
}

HALON_EXPORT
bool Halon_hsl_register(HalonHSLRegisterContext* ptr)
{
	HalonMTA_hsl_register_function(ptr, "mysql_query", &halon_mysql_query);
	HalonMTA_hsl_register_function(ptr, "mysql_escape_string", &halon_mysql_escape_string);
	return true;
}
