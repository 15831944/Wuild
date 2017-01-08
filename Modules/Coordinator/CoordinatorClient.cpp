/*
 * Copyright (C) 2017 Smirnov Vladimir mapron1@gmail.com
 * Source code licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0 or in file COPYING-APACHE-2.0.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.h
 */

#include "CoordinatorClient.h"

#include "CoordinatorFrames.h"

#include <SocketFrameService.h>
#include <ThreadUtils.h>

#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <fstream>

namespace Wuild
{

CoordinatorClient::CoordinatorClient()
{
}

CoordinatorClient::~CoordinatorClient()
{
	if (m_client)
		m_client->Stop();

	m_thread.Stop();
}

bool CoordinatorClient::SetConfig(const CoordinatorClient::Config &config)
{
	std::ostringstream os;
	if (!config.Validate(&os))
	{
		Syslogger(LOG_ERR) << os.str();
		return false;
	}
	m_config = config;
	return true;
}

void CoordinatorClient::SetToolServerChangeCallback(CoordinatorClient::ToolServerChangeCallback callback)
{
	m_toolServerChangeCallback = callback;
}

void CoordinatorClient::SetInfoArrivedCallback(CoordinatorClient::InfoArrivedCallback callback)
{
	m_infoArrivedCallback = callback;
}

void CoordinatorClient::Start()
{
	if (!m_config.m_enabled || m_config.m_coordinatorPort <= 0)
		return;

	m_client.reset(new SocketFrameHandler( ));
	m_client->RegisterFrameReader(SocketFrameReaderTemplate<CoordinatorListResponse>::Create([this](const CoordinatorListResponse& inputMessage, SocketFrameHandler::OutputCallback){
		 std::lock_guard<std::mutex> lock(m_coordMutex);

		 Syslogger(this->m_config.m_logContext) << " list arrived [" << inputMessage.m_info.m_toolServers.size() << "]";
		 auto modified = m_coord.Update(inputMessage.m_info.m_toolServers);
		 if (!modified.empty() && m_toolServerChangeCallback)
		 {
			 for (const ToolServerInfo * toolServer: modified)
				 m_toolServerChangeCallback(*toolServer);

		 }
		 if (m_infoArrivedCallback)
			 m_infoArrivedCallback(m_coord, inputMessage.m_latestSessions);
	}));
	m_client->SetChannelNotifier([this](bool state){
		m_clientState = state;
		if (!state)
			m_needRequestData = true;
	});

	m_client->SetTcpChannel(m_config.m_coordinatorHost, m_config.m_coordinatorPort);

	m_client->Start();

	m_thread.Exec(std::bind(&CoordinatorClient::Quant, this));
}

void CoordinatorClient::SetToolServerInfo(const ToolServerInfo &info)
{
	std::lock_guard<std::mutex> lock(m_toolServerInfoMutex);
	if (m_toolServerInfo == info)
		return;
	m_needSendToolServerInfo = true;
	m_toolServerInfo = info;
}

void CoordinatorClient::SendToolServerSessionInfo(const ToolServerSessionInfo &sessionInfo)
{
	if (!m_client)
		return;
	Syslogger(this->m_config.m_logContext) << " sending session " <<  sessionInfo.m_clientId;

	CoordinatorToolServerSession::Ptr message(new CoordinatorToolServerSession());
	message->m_session = sessionInfo;
	m_client->QueueFrame(message);
}

void CoordinatorClient::Quant()
{
	if (!m_clientState)
		return;

	if (m_config.m_sendInfoInterval && m_needSendToolServerInfo)
	{
		if (!m_lastSend || m_lastSend.GetElapsedTime() > m_config.m_sendInfoInterval)
		{
			m_lastSend = TimePoint(true);
			m_needSendToolServerInfo = false;
			{
				std::lock_guard<std::mutex> lock(m_toolServerInfoMutex);
				Syslogger(this->m_config.m_logContext) << " sending tool server " <<  m_toolServerInfo.m_toolServerId;
				CoordinatorToolServerStatus::Ptr message(new CoordinatorToolServerStatus());
				message->m_info = m_toolServerInfo;
				m_client->QueueFrame(message);
			}
		}
	}
	if (m_needRequestData)
	{
		m_needRequestData = false;
		m_client->QueueFrame(CoordinatorListRequest::Ptr(new CoordinatorListRequest()));
	}
}

}
