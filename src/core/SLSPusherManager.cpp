
/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2020 Edward.Wu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <string.h>
#include "spdlog/spdlog.h"

#include "common.hpp"
#include "SLSPusherManager.hpp"
#include "SLSLog.hpp"
#include "SLSPusher.hpp"

/**
 * CSLSPusherManager class implementation
 */

CSLSPusherManager::CSLSPusherManager()
{
}

CSLSPusherManager::~CSLSPusherManager()
{
}

// start to connect from next of cur index per time.
int CSLSPusherManager::connect_all()
{
	int ret = SLS_ERROR;
	if (m_sri == NULL)
	{
		spdlog::info("[{}] CSLSPusherManager::connect_all, failed, m_upstreams.size()=0, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_app_uplive, m_stream_name);
		return ret;
	}

	int all_ret = SLS_OK;
	for (unsigned int i = 0; i < m_sri->m_upstreams.size(); i++)
	{
		char szURL[1024] = {0};
		const char *szTmp = m_sri->m_upstreams[i].c_str();
		// Check if the srt:// prefix is already specified
		bool endpoint_load_success = false;
		try
		{
			if (strncmp(szTmp, "srt://", 6) == 0)
			{
				snprintf(szURL, sizeof(szURL), "%s", fmt::format(szTmp, fmt::arg("stream_name", m_stream_name)).c_str());
			}
			else
			{
				snprintf(szURL, sizeof(szURL), "srt://%s", fmt::format(szTmp, fmt::arg("stream_name", m_stream_name)).c_str());
			}
			endpoint_load_success = true;
		}
		catch (fmt::v9::format_error const &error)
		{
			spdlog::error("[{}] CSLSPusherManager::connect_all key '{{stream_name}}' not found in entry '{}'",
						  fmt::ptr(this), szTmp);
			// If argument is not found, notify of failure and don't try to reconnect
			ret = SLS_ERROR;
		}

		if (endpoint_load_success)
		{
			ret = connect(szURL);
			if (SLS_OK != ret)
			{
				CSLSLock lock(&m_rwclock, true);
				m_map_reconnect_relay[std::string(szURL)] = sls_gettime_ms();
			}
		}
		all_ret |= ret;
	}
	return all_ret;
}

int CSLSPusherManager::start()
{
	int ret = SLS_ERROR;
	if (m_sri == NULL)
	{
		spdlog::info("[{}] CSLSPusherManager::start, failed, m_upstreams.size()=0, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_app_uplive, m_stream_name);
		return ret;
	}

	// check publisher
	char key_stream_name[URL_MAX_LEN] = {0};
	ret = snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", m_app_uplive, m_stream_name);
	if (ret < 0 || (unsigned)ret >= sizeof(key_stream_name))
	{
		spdlog::error("[{}] CSLSPusherManager::start, failed, snprintf key_stream_name failed, ret={}, m_app_uplive={}, m_stream_name={}.",
					  fmt::ptr(this), ret, m_app_uplive, m_stream_name);
		return SLS_ERROR;
	}
	if (NULL != m_map_publisher)
	{
		CSLSRole *publisher = m_map_publisher->get_publisher(key_stream_name);
		if (NULL == publisher)
		{
			spdlog::info("[{}] CSLSPullerManager::start, failed, key_stream_name={}, publisher=NULL not exist.",
						 fmt::ptr(this), key_stream_name);
			return SLS_ERROR;
		}
	}

	if (SLS_PM_ALL == m_sri->m_mode)
	{
		return connect_all();
	}
	else if (SLS_PM_HASH == m_sri->m_mode)
	{
		ret = connect_hash();
	}
	else
	{
		spdlog::info("[{}] CSLSPusherManager::start, failed, wrong m_sri->m_mode={:d}, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_sri->m_mode, m_app_uplive, m_stream_name);
	}
	return ret;
}

CSLSRelay *CSLSPusherManager::create_relay()
{
	CSLSRelay *relay = new CSLSPusher;
	return relay;
}

int CSLSPusherManager::set_relay_param(CSLSRelay *relay)
{
	int ret;
	char key_stream_name[1024] = {0};

	ret = snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", m_app_uplive, m_stream_name);
	if (ret < 0 || (unsigned)ret >= sizeof(key_stream_name))
	{
		spdlog::error("[{}] CSLSPusherManager::set_relay_param, failed, snprintf key_stream_name failed, ret={}, m_app_uplive={}, m_stream_name={}.",
					  fmt::ptr(this), ret, m_app_uplive, m_stream_name);
		return SLS_ERROR;
	}
	relay->set_map_data(key_stream_name, m_map_data);
	relay->set_map_publisher(m_map_publisher);
	relay->set_relay_manager(this);
	m_role_list->push(relay);
	return SLS_OK;
}

int CSLSPusherManager::add_reconnect_stream(char *relay_url)
{
	int ret = SLS_ERROR;
	if (m_sri == NULL)
	{
		spdlog::info("[{}] CSLSPusherManager::add_reconnect_stream, failed, m_upstreams.size()=0, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_app_uplive, m_stream_name);
		return ret;
	}

	if (SLS_PM_ALL == m_sri->m_mode)
	{
		std::string url = std::string(relay_url);
		CSLSLock lock(&m_rwclock, true);
		int64_t tm = sls_gettime_ms();
		m_map_reconnect_relay[url] = tm;
		ret = SLS_OK;
	}
	else if (SLS_PM_HASH == m_sri->m_mode)
	{
		m_reconnect_begin_tm = sls_gettime_ms();
		ret = SLS_OK;
	}
	else
	{
		spdlog::info("[{}] CSLSPusherManager::add_reconnect_stream, failed, wrong m_sri->m_mode={:d}, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_sri->m_mode, m_app_uplive, m_stream_name);
	}
	return ret;
}

int CSLSPusherManager::reconnect(int64_t cur_tm_ms)
{
	int ret = SLS_ERROR;
	if (SLS_OK != check_relay_param())
	{
		spdlog::warn("[{}] CSLSPusherManager::reconnect, check_relay_param failed, stream={}.",
					 fmt::ptr(this), m_stream_name);
		return ret;
	}

	if (m_sri == NULL)
	{
		spdlog::info("[{}] CSLSPusherManager::reconnect, failed, m_upstreams.size()=0, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_app_uplive, m_stream_name);
		return ret;
	}

	// check publisher
	bool no_publisher = false;
	char key_stream_name[URL_MAX_LEN] = {0};
	ret = snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", m_app_uplive, m_stream_name);
	if (ret < 0 || (unsigned)ret >= sizeof(key_stream_name))
	{
		spdlog::error("[{}] CSLSPusherManager::reconnect, failed, snprintf key_stream_name failed, ret={}, m_app_uplive={}, m_stream_name={}.",
					  fmt::ptr(this), ret, m_app_uplive, m_stream_name);
		return SLS_ERROR;
	}
	if (NULL != m_map_publisher)
	{
		CSLSRole *publisher = m_map_publisher->get_publisher(key_stream_name);
		if (NULL == publisher)
		{
			no_publisher = true;
		}
	}

	if (SLS_PM_ALL == m_sri->m_mode)
	{
		ret = reconnect_all(cur_tm_ms, no_publisher);
	}
	else if (SLS_PM_HASH == m_sri->m_mode)
	{
		if (cur_tm_ms - m_reconnect_begin_tm < (m_sri->m_reconnect_interval * 1000))
		{
			return SLS_ERROR;
		}
		m_reconnect_begin_tm = cur_tm_ms;
		if (no_publisher)
		{
			spdlog::info("[{}] CSLSPullerManager::reconnect, connect_hash failed, key_stream_name={}, publisher=NULL not exist.",
						 fmt::ptr(this), key_stream_name);
			return SLS_ERROR;
		}
		ret = connect_hash();
	}
	else
	{
		spdlog::info("[{}] CSLSPusherManager::reconnect, failed, wrong m_sri->m_mode={:d}, m_app_uplive={}, m_stream_name={}.",
					 fmt::ptr(this), m_sri->m_mode, m_app_uplive, m_stream_name);
		return SLS_ERROR;
	}
	return ret;
}

int CSLSPusherManager::check_relay_param()
{
	if (NULL == m_role_list)
	{
		spdlog::warn("[{}] CSLSRelayManager::check_relay_param, failed, m_role_list is null, stream={}.",
					 fmt::ptr(this), m_stream_name);
		return SLS_ERROR;
	}
	if (NULL == m_map_data)
	{
		spdlog::warn("[{}] CSLSRelayManager::check_relay_param, failed, m_map_data is null, stream={}.",
					 fmt::ptr(this), m_stream_name);
		return SLS_ERROR;
	}
	return SLS_OK;
}

int CSLSPusherManager::reconnect_all(int64_t cur_tm_ms, bool no_publisher)
{
	CSLSLock lock(&m_rwclock, true);

	int ret = SLS_ERROR;
	int all_ret = SLS_OK;
	std::map<std::string, int64_t>::iterator it_cur;
	std::map<std::string, int64_t>::iterator it;
	for (it = m_map_reconnect_relay.begin(); it != m_map_reconnect_relay.end();)
	{
		std::string url = it->first;
		int64_t begin_tm = it->second;
		it_cur = it;
		it++;
		if (cur_tm_ms - begin_tm < (m_sri->m_reconnect_interval * 1000))
		{
			all_ret |= ret;
			continue;
		}
		if (no_publisher)
		{
			// it_cur->second = cur_tm_ms；
			all_ret |= ret;
			m_map_reconnect_relay[url] = cur_tm_ms;
			spdlog::info("[{}] CSLSPullerManager::reconnect_all, failed, url={}, publisher=NULL not exist.",
						 fmt::ptr(this), url.c_str());
			continue;
		}
		ret = connect(url.c_str());
		if (SLS_OK != ret)
		{
			spdlog::info("[{}] CSLSRelayManager::reconnect_all, faild, connect url='{}'.",
						 fmt::ptr(this), url.c_str());
			m_map_reconnect_relay[url] = cur_tm_ms;
		}
		else
		{
			spdlog::info("[{}] CSLSRelayManager::reconnect_all, ok, connect url='{}', erase item from m_map_reconnect_relay.",
						 fmt::ptr(this), url.c_str());
			m_map_reconnect_relay.erase(it_cur);
		}
		all_ret |= ret;
	}

	return all_ret;
}
