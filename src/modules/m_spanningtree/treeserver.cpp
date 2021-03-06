/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2007-2008 Craig Edwards <craigedwards@brainbox.cc>
 *   Copyright (C) 2008 Robin Burchell <robin+git@viroteck.net>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"
#include "xline.h"
#include "main.h"
#include "modules/spanningtree.h"

#include "utils.h"
#include "treeserver.h"

/** We use this constructor only to create the 'root' item, Utils->TreeRoot, which
 * represents our own server. Therefore, it has no route, no parent, and
 * no socket associated with it. Its version string is our own local version.
 */
TreeServer::TreeServer()
	: Server(ServerInstance->Config->ServerName, ServerInstance->Config->ServerDesc)
	, Parent(NULL), Route(NULL)
	, VersionString(ServerInstance->GetVersionString())
	, fullversion(ServerInstance->GetVersionString(true))
	, Socket(NULL), sid(ServerInstance->Config->GetSID()), behind_bursting(0), isdead(false)
	, ServerUser(ServerInstance->FakeClient)
	, age(ServerInstance->Time()), Warned(false), UserCount(ServerInstance->Users.GetLocalUsers().size())
	, OperCount(0), rtt(0), StartBurst(0), Hidden(false)
{
	AddHashEntry();
}

/** When we create a new server, we call this constructor to initialize it.
 * This constructor initializes the server's Route and Parent, and sets up
 * its ping counters so that it will be pinged one minute from now.
 */
TreeServer::TreeServer(const std::string& Name, const std::string& Desc, const std::string& id, TreeServer* Above, TreeSocket* Sock, bool Hide)
	: Server(Name, Desc)
	, Parent(Above), Socket(Sock), sid(id), behind_bursting(Parent->behind_bursting), isdead(false)
	, ServerUser(new FakeUser(id, this))
	, age(ServerInstance->Time()), Warned(false), UserCount(0), OperCount(0), rtt(0), StartBurst(0), Hidden(Hide)
{
	ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "New server %s behind_bursting %u", GetName().c_str(), behind_bursting);
	CheckULine();
	SetNextPingTime(ServerInstance->Time() + Utils->PingFreq);
	SetPingFlag();

	/* find the 'route' for this server (e.g. the one directly connected
	 * to the local server, which we can use to reach it)
	 *
	 * In the following example, consider we have just added a TreeServer
	 * class for server G on our network, of which we are server A.
	 * To route traffic to G (marked with a *) we must send the data to
	 * B (marked with a +) so this algorithm initializes the 'Route'
	 * value to point at whichever server traffic must be routed through
	 * to get here. If we were to try this algorithm with server B,
	 * the Route pointer would point at its own object ('this').
	 *
	 *            A
	 *           / \
	 *        + B   C
	 *         / \   \
	 *        D   E   F
	 *       /         \
	 *    * G           H
	 *
	 * We only run this algorithm when a server is created, as
	 * the routes remain constant while ever the server exists, and
	 * do not need to be re-calculated.
	 */

	Route = Above;
	if (Route == Utils->TreeRoot)
	{
		Route = this;
	}
	else
	{
		while (this->Route->GetParent() != Utils->TreeRoot)
		{
			this->Route = Route->GetParent();
		}
	}

	/* Because recursive code is slow and takes a lot of resources,
	 * we store two representations of the server tree. The first
	 * is a recursive structure where each server references its
	 * children and its parent, which is used for netbursts and
	 * netsplits to dump the whole dataset to the other server,
	 * and the second is used for very fast lookups when routing
	 * messages and is instead a hash_map, where each item can
	 * be referenced by its server name. The AddHashEntry()
	 * call below automatically inserts each TreeServer class
	 * into the hash_map as it is created. There is a similar
	 * maintainance call in the destructor to tidy up deleted
	 * servers.
	 */

	this->AddHashEntry();
	Parent->AddChild(this);
}

void TreeServer::BeginBurst(unsigned long startms)
{
	behind_bursting++;

	unsigned long now = ServerInstance->Time() * 1000 + (ServerInstance->Time_ns() / 1000000);
	// If the start time is in the future (clocks are not synced) then use current time
	if ((!startms) || (startms > now))
		startms = now;
	this->StartBurst = startms;
	ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Server %s started bursting at time %lu behind_bursting %u", sid.c_str(), startms, behind_bursting);
}

const std::string& TreeServer::GetID()
{
	return sid;
}

void TreeServer::FinishBurstInternal()
{
	// Check is needed because 1202 protocol servers don't send the bursting state of a server, so servers
	// introduced during a netburst may later send ENDBURST which would normally decrease this counter
	if (behind_bursting > 0)
		behind_bursting--;
	ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "FinishBurstInternal() %s behind_bursting %u", GetName().c_str(), behind_bursting);

	if (!IsBehindBursting())
	{
		SetNextPingTime(ServerInstance->Time() + Utils->PingFreq);
		SetPingFlag();
	}
	for (ChildServers::const_iterator i = Children.begin(); i != Children.end(); ++i)
	{
		TreeServer* child = *i;
		child->FinishBurstInternal();
	}
}

void TreeServer::FinishBurst()
{
	ServerInstance->XLines->ApplyLines();
	long ts = ServerInstance->Time() * 1000 + (ServerInstance->Time_ns() / 1000000);
	unsigned long bursttime = ts - this->StartBurst;
	ServerInstance->SNO->WriteToSnoMask(Parent == Utils->TreeRoot ? 'l' : 'L', "Received end of netburst from \2%s\2 (burst time: %lu %s)",
		GetName().c_str(), (bursttime > 10000 ? bursttime / 1000 : bursttime), (bursttime > 10000 ? "secs" : "msecs"));
	AddServerEvent(Utils->Creator, GetName());

	StartBurst = 0;
	FinishBurstInternal();
}

void TreeServer::SQuitChild(TreeServer* server, const std::string& reason)
{
	DelServerEvent(Utils->Creator, server->GetName());
	DelChild(server);

	if (IsRoot())
	{
		// Server split from us, generate a SQUIT message and broadcast it
		ServerInstance->SNO->WriteGlobalSno('l', "Server \002" + server->GetName() + "\002 split: " + reason);
		CmdBuilder("SQUIT").push(server->GetID()).push_last(reason).Broadcast();
	}
	else
	{
		ServerInstance->SNO->WriteToSnoMask('L', "Server \002" + server->GetName() + "\002 split from server \002" + GetName() + "\002 with reason: " + reason);
	}

	unsigned int num_lost_servers = 0;
	server->SQuitInternal(num_lost_servers);

	const std::string quitreason = GetName() + " " + server->GetName();
	unsigned int num_lost_users = QuitUsers(quitreason);

	ServerInstance->SNO->WriteToSnoMask(IsRoot() ? 'l' : 'L', "Netsplit complete, lost \002%u\002 user%s on \002%u\002 server%s.",
		num_lost_users, num_lost_users != 1 ? "s" : "", num_lost_servers, num_lost_servers != 1 ? "s" : "");

	// No-op if the socket is already closed (i.e. it called us)
	if (server->IsLocal())
		server->GetSocket()->Close();

	// Add the server to the cull list, the servers behind it are handled by cull() and the destructor
	ServerInstance->GlobalCulls.AddItem(server);
}

void TreeServer::SQuitInternal(unsigned int& num_lost_servers)
{
	ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Server %s lost in split", GetName().c_str());

	for (ChildServers::const_iterator i = Children.begin(); i != Children.end(); ++i)
	{
		TreeServer* server = *i;
		server->SQuitInternal(num_lost_servers);
	}

	// Mark server as dead
	isdead = true;
	num_lost_servers++;
	RemoveHash();
}

unsigned int TreeServer::QuitUsers(const std::string& reason)
{
	std::string publicreason = ServerInstance->Config->HideSplits ? "*.net *.split" : reason;

	const user_hash& users = ServerInstance->Users->GetUsers();
	unsigned int original_size = users.size();
	for (user_hash::const_iterator i = users.begin(); i != users.end(); )
	{
		User* user = i->second;
		// Increment the iterator now because QuitUser() removes the user from the container
		++i;
		TreeServer* server = TreeServer::Get(user);
		if (server->IsDead())
			ServerInstance->Users->QuitUser(user, publicreason, &reason);
	}
	return original_size - users.size();
}

void TreeServer::CheckULine()
{
	uline = silentuline = false;

	ConfigTagList tags = ServerInstance->Config->ConfTags("uline");
	for (ConfigIter i = tags.first; i != tags.second; ++i)
	{
		ConfigTag* tag = i->second;
		std::string server = tag->getString("server");
		if (!strcasecmp(server.c_str(), GetName().c_str()))
		{
			if (this->IsRoot())
			{
				ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Servers should not uline themselves (at " + tag->getTagLocation() + ")");
				return;
			}

			uline = true;
			silentuline = tag->getBool("silent");
			break;
		}
	}
}

/** This method is used to add the structure to the
 * hash_map for linear searches. It is only called
 * by the constructors.
 */
void TreeServer::AddHashEntry()
{
	Utils->serverlist[GetName()] = this;
	Utils->sidlist[sid] = this;
}

/** These accessors etc should be pretty self-
 * explanitory.
 */
TreeServer* TreeServer::GetRoute()
{
	return Route;
}

const std::string& TreeServer::GetVersion()
{
	return VersionString;
}

void TreeServer::SetNextPingTime(time_t t)
{
	this->NextPing = t;
	LastPingWasGood = false;
}

time_t TreeServer::NextPingTime()
{
	return NextPing;
}

bool TreeServer::AnsweredLastPing()
{
	return LastPingWasGood;
}

void TreeServer::SetPingFlag()
{
	LastPingWasGood = true;
}

TreeSocket* TreeServer::GetSocket()
{
	return Socket;
}

TreeServer* TreeServer::GetParent()
{
	return Parent;
}

void TreeServer::SetVersion(const std::string &Version)
{
	VersionString = Version;
}

void TreeServer::AddChild(TreeServer* Child)
{
	Children.push_back(Child);
}

bool TreeServer::DelChild(TreeServer* Child)
{
	return stdalgo::erase(Children, Child);
}

CullResult TreeServer::cull()
{
	// Recursively cull all servers that are under us in the tree
	for (ChildServers::const_iterator i = Children.begin(); i != Children.end(); ++i)
	{
		TreeServer* server = *i;
		server->cull();
	}

	if (!IsRoot())
		ServerUser->cull();
	return classbase::cull();
}

TreeServer::~TreeServer()
{
	// Recursively delete all servers that are under us in the tree first
	for (ChildServers::const_iterator i = Children.begin(); i != Children.end(); ++i)
		delete *i;

	// Delete server user unless it's us
	if (!IsRoot())
		delete ServerUser;
}

void TreeServer::RemoveHash()
{
	// XXX: Erase server from UserManager::uuidlist now, to allow sid reuse in the current main loop
	// iteration, before the cull list is applied
	ServerInstance->Users->uuidlist.erase(sid);

	Utils->sidlist.erase(sid);
	Utils->serverlist.erase(GetName());
}
