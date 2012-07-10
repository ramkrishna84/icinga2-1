/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-checker.h"

using namespace icinga;

string CheckerComponent::GetName(void) const
{
	return "checker";
}

void CheckerComponent::Start(void)
{
	m_Endpoint = boost::make_shared<VirtualEndpoint>();
	m_Endpoint->RegisterTopicHandler("checker::AssignService",
		boost::bind(&CheckerComponent::AssignServiceRequestHandler, this, _2, _3));
	m_Endpoint->RegisterTopicHandler("checker::ClearServices",
		boost::bind(&CheckerComponent::ClearServicesRequestHandler, this, _2, _3));
	m_Endpoint->RegisterPublication("checker::CheckResult");
	EndpointManager::GetInstance()->RegisterEndpoint(m_Endpoint);

	m_CheckTimer = boost::make_shared<Timer>();
	m_CheckTimer->SetInterval(5);
	m_CheckTimer->OnTimerExpired.connect(boost::bind(&CheckerComponent::CheckTimerHandler, this));
	m_CheckTimer->Start();

	NagiosCheckTask::Register();

	m_ResultTimer = boost::make_shared<Timer>();
	m_ResultTimer->SetInterval(5);
	m_ResultTimer->OnTimerExpired.connect(boost::bind(&CheckerComponent::ResultTimerHandler, this));
	m_ResultTimer->Start();
}

void CheckerComponent::Stop(void)
{
	EndpointManager::Ptr mgr = EndpointManager::GetInstance();

	if (mgr)
		mgr->UnregisterEndpoint(m_Endpoint);
}

void CheckerComponent::CheckTimerHandler(void)
{
	time_t now;
	time(&now);

	Logger::Write(LogDebug, "checker", "CheckTimerHandler entered.");

	long tasks = 0;

	while (!m_Services.empty()) {
		Service service = m_Services.top();

		if (service.GetNextCheck() > now)
			break;

		m_Services.pop();

		Logger::Write(LogDebug, "checker", "Executing service check for '" + service.GetName() + "'");

		m_PendingServices.insert(service.GetConfigObject());

		CheckTask::Ptr task = CheckTask::CreateTask(service);
		task->Enqueue();

		tasks++;
	}

	Logger::Write(LogDebug, "checker", "CheckTimerHandler: past loop.");

	CheckTask::FlushQueue();

	stringstream msgbuf;
	msgbuf << "CheckTimerHandler: created " << tasks << " tasks";
	Logger::Write(LogInformation, "checker", msgbuf.str());
}

void CheckerComponent::ResultTimerHandler(void)
{
	Logger::Write(LogDebug, "checker", "ResultTimerHandler entered.");

	time_t now;
	time(&now);

	long min_latency = -1, max_latency = 0, avg_latency = 0, results = 0, failed = 0;

	vector<CheckTask::Ptr> finishedTasks = CheckTask::GetFinishedTasks();

	for (vector<CheckTask::Ptr>::iterator it = finishedTasks.begin(); it != finishedTasks.end(); it++) {
		CheckTask::Ptr task = *it;

		Service service = task->GetService();

		/* if the service isn't in the set of pending services
		 * it was removed and we need to ignore this check result. */
		if (m_PendingServices.find(service.GetConfigObject()) == m_PendingServices.end())
			continue;

		CheckResult result = task->GetResult();
		Logger::Write(LogDebug, "checker", "Got result for service '" + service.GetName() + "'");

		long execution_time = result.GetExecutionEnd() - result.GetExecutionStart();
		long latency = (result.GetScheduleEnd() - result.GetScheduleStart()) - execution_time;
		avg_latency += latency;

		if (min_latency == -1 || latency < min_latency)
			min_latency = latency;

		if (latency > max_latency)
			max_latency = latency;

		results++;

		if (result.GetState() != StateOK)
			failed++;

		/* update service state */
		service.ApplyCheckResult(result);

		/* figure out when the next check is for this service */
		service.UpdateNextCheck();

		/* remove the service from the list of pending services */
		m_PendingServices.erase(service.GetConfigObject());
		m_Services.push(service);

		RequestMessage rm;
		rm.SetMethod("checker::CheckResult");

		ServiceStatusMessage params;
		params.SetService(service.GetName());
		params.SetState(service.GetState());
		params.SetStateType(service.GetStateType());
		params.SetCurrentCheckAttempt(service.GetCurrentCheckAttempt());
		params.SetNextCheck(service.GetNextCheck());
		params.SetCheckResult(result);

		rm.SetParams(params);

		EndpointManager::GetInstance()->SendMulticastMessage(m_Endpoint, rm);
	}

	if (min_latency > 5) {
		stringstream latwarn;
		latwarn << "We can't keep up with the checks: minimum latency is " << min_latency << " seconds";
		Logger::Write(LogWarning, "checker", latwarn.str());
	}

	{
		stringstream msgbuf;
		msgbuf << "ResultTimerHandler: " << results << " results (" << failed << " failed); latency: avg=" << avg_latency / (results ? results : 1) << ", min=" << min_latency << ", max: " << max_latency;
		Logger::Write(LogInformation, "checker", msgbuf.str());
	}

	{
		stringstream msgbuf;
		msgbuf << "Pending services: " << m_PendingServices.size() << "; Idle services: " << m_Services.size();
		Logger::Write(LogInformation, "checker", msgbuf.str());
	}
}

void CheckerComponent::AssignServiceRequestHandler(const Endpoint::Ptr& sender, const RequestMessage& request)
{
	MessagePart params;
	if (!request.GetParams(&params))
		return;

	MessagePart serviceMsg;
	if (!params.Get("service", &serviceMsg))
		return;

	ConfigObject::Ptr object = boost::make_shared<ConfigObject>(serviceMsg.GetDictionary());
	Service service(object);
	m_Services.push(service);

	Logger::Write(LogDebug, "checker", "Accepted delegation for service '" + service.GetName() + "'");

	string id;
	if (request.GetID(&id)) {
		ResponseMessage rm;
		rm.SetID(id);

		MessagePart result;
		rm.SetResult(result);
		EndpointManager::GetInstance()->SendUnicastMessage(m_Endpoint, sender, rm);
	}
}

void CheckerComponent::ClearServicesRequestHandler(const Endpoint::Ptr& sender, const RequestMessage& request)
{
	Logger::Write(LogInformation, "checker", "Clearing service delegations.");

	/* clear the services lists */
	m_Services = ServiceQueue();
	m_PendingServices.clear();

	/* TODO: clear checks we've already sent to the thread pool */
}

EXPORT_COMPONENT(checker, CheckerComponent);
