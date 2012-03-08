/** 
 * @file llpathfindingmanager.cpp
 * @author William Todd Stinson
 * @brief A state manager for the various pathfinding states.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include <string>
#include <vector>

#include "llviewerprecompiledheaders.h"
#include "llsd.h"
#include "llpathfindingmanager.h"
#include "llsingleton.h"
#include "llhttpclient.h"
#include "llagent.h"
#include "llviewerregion.h"
#include "llpathfindingnavmesh.h"
#include "llpathfindinglinkset.h"
#include "llpathfindinglinksetlist.h"
#include "llhttpnode.h"

#include <boost/function.hpp>
#include <boost/signals2.hpp>

#define CAP_SERVICE_RETRIEVE_NAVMESH  "RetrieveNavMeshSrc"

#define CAP_SERVICE_AGENT_STATE       "AgentPreferences"
#define ALTER_NAVMESH_OBJECTS_FIELD "alter_navmesh_objects"
#define DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD "alter_permanent_objects"

#define CAP_SERVICE_OBJECT_LINKSETS   "ObjectNavMeshProperties"
#define CAP_SERVICE_TERRAIN_LINKSETS  "TerrainNavMeshProperties"

#define SIM_MESSAGE_NAVMESH_STATUS_UPDATE "/message/NavmeshStatusUpdate"

//---------------------------------------------------------------------------
// LLNavMeshSimStateChangeNode
//---------------------------------------------------------------------------

class LLNavMeshSimStateChangeNode : public LLHTTPNode
{
public:
	virtual void post(ResponsePtr pResponse, const LLSD &pContext, const LLSD &pInput) const;
};

LLHTTPRegistration<LLNavMeshSimStateChangeNode> gHTTPRegistrationNavMeshSimStateChangeNode(SIM_MESSAGE_NAVMESH_STATUS_UPDATE);

//---------------------------------------------------------------------------
// NavMeshResponder
//---------------------------------------------------------------------------

class NavMeshResponder : public LLHTTPClient::Responder
{
public:
	NavMeshResponder(const std::string &pCapabilityURL, U32 pNavMeshVersion, LLPathfindingNavMeshPtr pNavMeshPtr);
	virtual ~NavMeshResponder();

	virtual void result(const LLSD &pContent);
	virtual void error(U32 pStatus, const std::string& pReason);

protected:

private:
	std::string             mCapabilityURL;
	U32                     mNavMeshVersion;
	LLPathfindingNavMeshPtr mNavMeshPtr;
};

//---------------------------------------------------------------------------
// AgentStateResponder
//---------------------------------------------------------------------------

class AgentStateResponder : public LLHTTPClient::Responder
{
public:
	AgentStateResponder(const std::string &pCapabilityURL, LLPathfindingManager::EAgentState pRequestedAgentState = LLPathfindingManager::kAgentStateUnknown);
	virtual ~AgentStateResponder();

	virtual void result(const LLSD &pContent);
	virtual void error(U32 pStatus, const std::string& pReason);

protected:

private:
	std::string                       mCapabilityURL;
	LLPathfindingManager::EAgentState mRequestedAgentState;
};

//---------------------------------------------------------------------------
// LinksetsResponder
//---------------------------------------------------------------------------

class LinksetsResponder
{
public:
	LinksetsResponder(LLPathfindingManager::linksets_callback_t pLinksetsCallback, bool pIsObjectRequested, bool pIsTerrainRequested);
	virtual ~LinksetsResponder();
	
	void handleObjectLinksetsResult(const LLSD &pContent);
	void handleObjectLinksetsError(U32 pStatus, const std::string &pReason, const std::string &pURL);
	void handleTerrainLinksetsResult(const LLSD &pContent);
	void handleTerrainLinksetsError(U32 pStatus, const std::string &pReason, const std::string &pURL);
	
protected:

private:
	void sendCallback();

	typedef enum
	{
		kNotRequested,
		kWaiting,
		kReceivedGood,
		kReceivedError
	} EMessagingState;
	
	LLPathfindingManager::linksets_callback_t mLinksetsCallback;
	
	EMessagingState                           mObjectMessagingState;
	EMessagingState                           mTerrainMessagingState;
	
	LLPathfindingLinksetListPtr               mObjectLinksetListPtr;
	LLPathfindingLinksetPtr                   mTerrainLinksetPtr;
};

typedef boost::shared_ptr<LinksetsResponder> LinksetsResponderPtr;

//---------------------------------------------------------------------------
// ObjectLinksetsResponder
//---------------------------------------------------------------------------

class ObjectLinksetsResponder : public LLHTTPClient::Responder
{
public:
	ObjectLinksetsResponder(const std::string &pCapabilityURL, LinksetsResponderPtr pLinksetsResponsderPtr);
	virtual ~ObjectLinksetsResponder();
	
	virtual void result(const LLSD &pContent);
	virtual void error(U32 pStatus, const std::string &pReason);
	
protected:
	
private:
	std::string          mCapabilityURL;
	LinksetsResponderPtr mLinksetsResponsderPtr;
};

//---------------------------------------------------------------------------
// TerrainLinksetsResponder
//---------------------------------------------------------------------------

class TerrainLinksetsResponder : public LLHTTPClient::Responder
{
public:
	TerrainLinksetsResponder(const std::string &pCapabilityURL, LinksetsResponderPtr pLinksetsResponsderPtr);
	virtual ~TerrainLinksetsResponder();
	
	virtual void result(const LLSD &pContent);
	virtual void error(U32 pStatus, const std::string &pReason);
	
protected:
	
private:
	std::string          mCapabilityURL;
	LinksetsResponderPtr mLinksetsResponsderPtr;
};

//---------------------------------------------------------------------------
// LLPathfindingManager
//---------------------------------------------------------------------------

LLPathfindingManager::LLPathfindingManager()
	: LLSingleton<LLPathfindingManager>(),
	mNavMeshMap(),
	mNavMeshVersionXXX(0),
	mAgentStateSignal(),
	mAgentState(kAgentStateUnknown),
	mLastKnownNonErrorAgentState(kAgentStateUnknown)
{
}

LLPathfindingManager::~LLPathfindingManager()
{
}

bool LLPathfindingManager::isPathfindingEnabledForCurrentRegion() const
{
	std::string retrieveNavMeshURL = getRetrieveNavMeshURLForCurrentRegion();
	return !retrieveNavMeshURL.empty();
}

bool LLPathfindingManager::isAllowAlterPermanent()
{
	return (!isPathfindingEnabledForCurrentRegion() || (getAgentState() == kAgentStateUnfrozen));
}

bool LLPathfindingManager::isAllowViewTerrainProperties() const
{
	LLViewerRegion* region = getCurrentRegion();
	return (gAgent.isGodlike() || ((region != NULL) && region->canManageEstate()));
}

LLPathfindingNavMesh::navmesh_slot_t LLPathfindingManager::registerNavMeshListenerForRegion(LLViewerRegion *pRegion, LLPathfindingNavMesh::navmesh_callback_t pNavMeshCallback)
{
	LLPathfindingNavMeshPtr navMeshPtr = getNavMeshForRegion(pRegion);
	return navMeshPtr->registerNavMeshListener(pNavMeshCallback);
}

void LLPathfindingManager::requestGetNavMeshForRegion(LLViewerRegion *pRegion)
{
	LLPathfindingNavMeshPtr navMeshPtr = getNavMeshForRegion(pRegion);

	if (navMeshPtr->hasNavMeshVersion(mNavMeshVersionXXX))
	{
		navMeshPtr->handleRefresh();
	}
	else
	{
		if (pRegion == NULL)
		{
			navMeshPtr->handleNavMeshNotEnabled();
		}
		else
		{
			std::string navMeshURL = getRetrieveNavMeshURLForRegion(pRegion);
			if (navMeshURL.empty())
			{
				navMeshPtr->handleNavMeshNotEnabled();
			}
			else
			{
				navMeshPtr->handleNavMeshStart(mNavMeshVersionXXX);
				LLHTTPClient::ResponderPtr responder = new NavMeshResponder(navMeshURL, mNavMeshVersionXXX, navMeshPtr);

				LLSD postData;
				LLHTTPClient::post(navMeshURL, postData, responder);
			}
		}
	}
}

void LLPathfindingManager::handleNavMeshUpdate(const LLUUID &pRegionUUID, U32 pNavMeshVersion)
{
	LLPathfindingNavMeshPtr navMeshPtr = getNavMeshForRegion(pRegionUUID);
	navMeshPtr->handleNavMeshNewVersion(++mNavMeshVersionXXX);
}

LLPathfindingManager::agent_state_slot_t LLPathfindingManager::registerAgentStateListener(agent_state_callback_t pAgentStateCallback)
{
	return mAgentStateSignal.connect(pAgentStateCallback);
}

LLPathfindingManager::EAgentState LLPathfindingManager::getAgentState()
{
	if (!isPathfindingEnabledForCurrentRegion())
	{
		setAgentState(kAgentStateNotEnabled);
	}
	else
	{
		if (!isValidAgentState(mAgentState))
		{
			requestGetAgentState();
		}
	}

	return mAgentState;
}

LLPathfindingManager::EAgentState LLPathfindingManager::getLastKnownNonErrorAgentState() const
{
	return mLastKnownNonErrorAgentState;
}

void LLPathfindingManager::requestSetAgentState(EAgentState pRequestedAgentState)
{
	llassert(isValidAgentState(pRequestedAgentState));
	std::string agentStateURL = getAgentStateURLForCurrentRegion();

	if (agentStateURL.empty())
	{
		setAgentState(kAgentStateNotEnabled);
	}
	else
	{
		LLSD request;
		request[ALTER_NAVMESH_OBJECTS_FIELD] = static_cast<LLSD::Boolean>(pRequestedAgentState == kAgentStateUnfrozen);
#ifdef DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD
		request[DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD] = static_cast<LLSD::Boolean>(pRequestedAgentState == kAgentStateUnfrozen);
#endif // DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD

		LLHTTPClient::ResponderPtr responder = new AgentStateResponder(agentStateURL, pRequestedAgentState);
		LLHTTPClient::post(agentStateURL, request, responder);
	}
}

LLPathfindingManager::ELinksetsRequestStatus LLPathfindingManager::requestGetLinksets(linksets_callback_t pLinksetsCallback) const
{
	ELinksetsRequestStatus status;

	std::string objectLinksetsURL = getObjectLinksetsURLForCurrentRegion();
	std::string terrainLinksetsURL = getTerrainLinksetsURLForCurrentRegion();
	if (objectLinksetsURL.empty() || terrainLinksetsURL.empty())
	{
		status = kLinksetsRequestNotEnabled;
	}
	else
	{
		bool doRequestTerrain = isAllowViewTerrainProperties();
		LinksetsResponderPtr linksetsResponderPtr(new LinksetsResponder(pLinksetsCallback, true, doRequestTerrain));
		
		LLHTTPClient::ResponderPtr objectLinksetsResponder = new ObjectLinksetsResponder(objectLinksetsURL, linksetsResponderPtr);
		LLHTTPClient::get(objectLinksetsURL, objectLinksetsResponder);

		if (doRequestTerrain)
		{
			LLHTTPClient::ResponderPtr terrainLinksetsResponder = new TerrainLinksetsResponder(terrainLinksetsURL, linksetsResponderPtr);
			LLHTTPClient::get(terrainLinksetsURL, terrainLinksetsResponder);
		}

		status = kLinksetsRequestStarted;
	}

	return status;
}

LLPathfindingManager::ELinksetsRequestStatus LLPathfindingManager::requestSetLinksets(LLPathfindingLinksetListPtr pLinksetList, LLPathfindingLinkset::ELinksetUse pLinksetUse, S32 pA, S32 pB, S32 pC, S32 pD, linksets_callback_t pLinksetsCallback) const
{
	ELinksetsRequestStatus status = kLinksetsRequestNotEnabled;

	std::string objectLinksetsURL = getObjectLinksetsURLForCurrentRegion();
	std::string terrainLinksetsURL = getTerrainLinksetsURLForCurrentRegion();
	if (objectLinksetsURL.empty() || terrainLinksetsURL.empty())
	{
		status = kLinksetsRequestNotEnabled;
	}
	else
	{
		LLSD objectPostData = pLinksetList->encodeObjectFields(pLinksetUse, pA, pB, pC, pD);
		LLSD terrainPostData;
		if (isAllowViewTerrainProperties())
		{
			terrainPostData = pLinksetList->encodeTerrainFields(pLinksetUse, pA, pB, pC, pD);
		}

		if (objectPostData.isUndefined() && terrainPostData.isUndefined())
		{
			status = kLinksetsRequestCompleted;
		}
		else
		{
			LinksetsResponderPtr linksetsResponderPtr(new LinksetsResponder(pLinksetsCallback, !objectPostData.isUndefined(), !terrainPostData.isUndefined()));

			if (!objectPostData.isUndefined())
			{
				LLHTTPClient::ResponderPtr objectLinksetsResponder = new ObjectLinksetsResponder(objectLinksetsURL, linksetsResponderPtr);
				LLHTTPClient::put(objectLinksetsURL, objectPostData, objectLinksetsResponder);
			}
			
			if (!terrainPostData.isUndefined())
			{
				LLHTTPClient::ResponderPtr terrainLinksetsResponder = new TerrainLinksetsResponder(terrainLinksetsURL, linksetsResponderPtr);
				LLHTTPClient::put(terrainLinksetsURL, terrainPostData, terrainLinksetsResponder);
			}

			status = kLinksetsRequestStarted;
		}
	}

	return status;
}

LLPathfindingNavMeshPtr LLPathfindingManager::getNavMeshForRegion(const LLUUID &pRegionUUID)
{
	LLPathfindingNavMeshPtr navMeshPtr;
	NavMeshMap::iterator navMeshIter = mNavMeshMap.find(pRegionUUID);
	if (navMeshIter == mNavMeshMap.end())
	{
		navMeshPtr = LLPathfindingNavMeshPtr(new LLPathfindingNavMesh(pRegionUUID));
		mNavMeshMap.insert(std::pair<LLUUID, LLPathfindingNavMeshPtr>(pRegionUUID, navMeshPtr));
	}
	else
	{
		navMeshPtr = navMeshIter->second;
	}

	return navMeshPtr;
}

LLPathfindingNavMeshPtr LLPathfindingManager::getNavMeshForRegion(LLViewerRegion *pRegion)
{
	LLUUID regionUUID;
	if (pRegion != NULL)
	{
		regionUUID = pRegion->getRegionID();
	}

	return getNavMeshForRegion(regionUUID);
}

bool LLPathfindingManager::isValidAgentState(EAgentState pAgentState)
{
	return ((pAgentState == kAgentStateFrozen) || (pAgentState == kAgentStateUnfrozen));
}

void LLPathfindingManager::requestGetAgentState()
{
	std::string agentStateURL = getAgentStateURLForCurrentRegion();

	if (agentStateURL.empty())
	{
		setAgentState(kAgentStateNotEnabled);
	}
	else
	{
		LLHTTPClient::ResponderPtr responder = new AgentStateResponder(agentStateURL);
		LLHTTPClient::get(agentStateURL, responder);
	}
}

void LLPathfindingManager::setAgentState(EAgentState pAgentState)
{
	mAgentState = pAgentState;

	if (mAgentState != kAgentStateError)
	{
		mLastKnownNonErrorAgentState = mAgentState;
	}

	mAgentStateSignal(mAgentState);
}

void LLPathfindingManager::handleAgentStateResult(const LLSD &pContent, EAgentState pRequestedAgentState)
{
#ifndef DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD
	llassert(pContent.has(ALTER_NAVMESH_OBJECTS_FIELD));
	llassert(pContent.get(ALTER_NAVMESH_OBJECTS_FIELD).isBoolean());
	EAgentState agentState = (pContent.get(ALTER_NAVMESH_OBJECTS_FIELD).asBoolean() ? kAgentStateUnfrozen : kAgentStateFrozen);
#else // DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD
	EAgentState agentState = kAgentStateUnknown;
	if (pContent.has(ALTER_NAVMESH_OBJECTS_FIELD))
	{
		llassert(pContent.get(ALTER_NAVMESH_OBJECTS_FIELD).isBoolean());
		agentState = (pContent.get(ALTER_NAVMESH_OBJECTS_FIELD).asBoolean() ? kAgentStateUnfrozen : kAgentStateFrozen);
	}
	else
	{
		llassert(pContent.has(DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD));
		llassert(pContent.get(DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD).isBoolean());
		agentState = (pContent.get(DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD).asBoolean() ? kAgentStateUnfrozen : kAgentStateFrozen);
	}
#endif // DEPRECATED_ALTER_NAVMESH_OBJECTS_FIELD

	if (isValidAgentState(pRequestedAgentState) && (agentState != pRequestedAgentState))
	{
		agentState = kAgentStateError;
		llassert(0);
	}

	setAgentState(agentState);
}

void LLPathfindingManager::handleAgentStateError(U32 pStatus, const std::string &pReason, const std::string &pURL)
{
	llwarns << "error with request to URL '" << pURL << "' because " << pReason << " (statusCode:" << pStatus << ")" << llendl;
	setAgentState(kAgentStateError);
}

std::string LLPathfindingManager::getRetrieveNavMeshURLForCurrentRegion() const
{
	return getCapabilityURLForCurrentRegion(CAP_SERVICE_RETRIEVE_NAVMESH);
}

std::string LLPathfindingManager::getRetrieveNavMeshURLForRegion(LLViewerRegion *pRegion) const
{
	return getCapabilityURLForRegion(pRegion, CAP_SERVICE_RETRIEVE_NAVMESH);
}

std::string LLPathfindingManager::getAgentStateURLForCurrentRegion() const
{
	return getCapabilityURLForCurrentRegion(CAP_SERVICE_AGENT_STATE);
}

std::string LLPathfindingManager::getObjectLinksetsURLForCurrentRegion() const
{
	return getCapabilityURLForCurrentRegion(CAP_SERVICE_OBJECT_LINKSETS);
}

std::string LLPathfindingManager::getTerrainLinksetsURLForCurrentRegion() const
{
	return getCapabilityURLForCurrentRegion(CAP_SERVICE_TERRAIN_LINKSETS);
}

std::string LLPathfindingManager::getCapabilityURLForCurrentRegion(const std::string &pCapabilityName) const
{
	return getCapabilityURLForRegion(getCurrentRegion(), pCapabilityName);
}

std::string LLPathfindingManager::getCapabilityURLForRegion(LLViewerRegion *pRegion, const std::string &pCapabilityName) const
{
	std::string capabilityURL("");

	if (pRegion != NULL)
	{
		capabilityURL = pRegion->getCapability(pCapabilityName);
	}

	if (capabilityURL.empty())
	{
		llwarns << "cannot find capability '" << pCapabilityName << "' for current region '"
			<< ((pRegion != NULL) ? pRegion->getName() : "<null>") << "'" << llendl;
	}

	return capabilityURL;
}

LLViewerRegion *LLPathfindingManager::getCurrentRegion() const
{
	return gAgent.getRegion();
}

//---------------------------------------------------------------------------
// LLNavMeshSimStateChangeNode
//---------------------------------------------------------------------------

void LLNavMeshSimStateChangeNode::post(ResponsePtr pResponse, const LLSD &pContext, const LLSD &pInput) const
{
	LLViewerRegion *region = gAgent.getRegion();
	U32 navMeshVersion = 0U;
	LLPathfindingManager::getInstance()->handleNavMeshUpdate(region->getRegionID(), navMeshVersion);
}

//---------------------------------------------------------------------------
// NavMeshResponder
//---------------------------------------------------------------------------

NavMeshResponder::NavMeshResponder(const std::string &pCapabilityURL, U32 pNavMeshVersion, LLPathfindingNavMeshPtr pNavMeshPtr)
	: mCapabilityURL(pCapabilityURL),
	mNavMeshVersion(pNavMeshVersion),
	mNavMeshPtr(pNavMeshPtr)
{
}

NavMeshResponder::~NavMeshResponder()
{
}

void NavMeshResponder::result(const LLSD &pContent)
{
	mNavMeshPtr->handleNavMeshResult(pContent, mNavMeshVersion);
}

void NavMeshResponder::error(U32 pStatus, const std::string& pReason)
{
	mNavMeshPtr->handleNavMeshError(pStatus, pReason, mCapabilityURL, mNavMeshVersion);
}

//---------------------------------------------------------------------------
// AgentStateResponder
//---------------------------------------------------------------------------

AgentStateResponder::AgentStateResponder(const std::string &pCapabilityURL, LLPathfindingManager::EAgentState pRequestedAgentState)
	: LLHTTPClient::Responder(),
	mCapabilityURL(pCapabilityURL),
	mRequestedAgentState(pRequestedAgentState)
{
}

AgentStateResponder::~AgentStateResponder()
{
}

void AgentStateResponder::result(const LLSD &pContent)
{
	LLPathfindingManager::getInstance()->handleAgentStateResult(pContent, mRequestedAgentState);
}

void AgentStateResponder::error(U32 pStatus, const std::string &pReason)
{
	LLPathfindingManager::getInstance()->handleAgentStateError(pStatus, pReason, mCapabilityURL);
}

//---------------------------------------------------------------------------
// LinksetsResponder
//---------------------------------------------------------------------------

LinksetsResponder::LinksetsResponder(LLPathfindingManager::linksets_callback_t pLinksetsCallback, bool pIsObjectRequested, bool pIsTerrainRequested)
	: mLinksetsCallback(pLinksetsCallback),
	mObjectMessagingState(pIsObjectRequested ? kWaiting : kNotRequested),
	mTerrainMessagingState(pIsTerrainRequested ? kWaiting : kNotRequested),
	mObjectLinksetListPtr(),
	mTerrainLinksetPtr()
{
}

LinksetsResponder::~LinksetsResponder()
{
}

void LinksetsResponder::handleObjectLinksetsResult(const LLSD &pContent)
{
	mObjectLinksetListPtr = LLPathfindingLinksetListPtr(new LLPathfindingLinksetList(pContent));

	mObjectMessagingState = kReceivedGood;
	if (mTerrainMessagingState != kWaiting)
	{
		sendCallback();
	}
}

void LinksetsResponder::handleObjectLinksetsError(U32 pStatus, const std::string &pReason, const std::string &pURL)
{
	llwarns << "error with request to URL '" << pURL << "' because " << pReason << " (statusCode:" << pStatus << ")" << llendl;
	mObjectMessagingState = kReceivedError;
	if (mTerrainMessagingState != kWaiting)
	{
		sendCallback();
	}
}

void LinksetsResponder::handleTerrainLinksetsResult(const LLSD &pContent)
{
	mTerrainLinksetPtr = LLPathfindingLinksetPtr(new LLPathfindingLinkset(pContent));

	mTerrainMessagingState = kReceivedGood;
	if (mObjectMessagingState != kWaiting)
	{
		sendCallback();
	}
}

void LinksetsResponder::handleTerrainLinksetsError(U32 pStatus, const std::string &pReason, const std::string &pURL)
{
	mTerrainMessagingState = kReceivedError;
	if (mObjectMessagingState != kWaiting)
	{
		sendCallback();
	}
}
	
void LinksetsResponder::sendCallback()
{
	llassert(mObjectMessagingState != kWaiting);
	llassert(mTerrainMessagingState != kWaiting);
	LLPathfindingManager::ELinksetsRequestStatus requestStatus =
		((((mObjectMessagingState == kReceivedGood) || (mObjectMessagingState == kNotRequested)) &&
		  ((mTerrainMessagingState == kReceivedGood) || (mTerrainMessagingState == kNotRequested))) ?
		 LLPathfindingManager::kLinksetsRequestCompleted : LLPathfindingManager::kLinksetsRequestError);

	if (mObjectMessagingState != kReceivedGood)
	{
		mObjectLinksetListPtr = LLPathfindingLinksetListPtr(new LLPathfindingLinksetList());
	}

	if (mTerrainMessagingState == kReceivedGood)
	{
		mObjectLinksetListPtr->insert(std::pair<std::string, LLPathfindingLinksetPtr>(mTerrainLinksetPtr->getUUID().asString(), mTerrainLinksetPtr));
	}

	mLinksetsCallback(requestStatus, mObjectLinksetListPtr);
}

//---------------------------------------------------------------------------
// ObjectLinksetsResponder
//---------------------------------------------------------------------------

ObjectLinksetsResponder::ObjectLinksetsResponder(const std::string &pCapabilityURL, LinksetsResponderPtr pLinksetsResponsderPtr)
	: mCapabilityURL(pCapabilityURL),
	mLinksetsResponsderPtr(pLinksetsResponsderPtr)
{
}

ObjectLinksetsResponder::~ObjectLinksetsResponder()
{
}

void ObjectLinksetsResponder::result(const LLSD &pContent)
{
	mLinksetsResponsderPtr->handleObjectLinksetsResult(pContent);
}

void ObjectLinksetsResponder::error(U32 pStatus, const std::string &pReason)
{
	mLinksetsResponsderPtr->handleObjectLinksetsError(pStatus, pReason, mCapabilityURL);
}
	
//---------------------------------------------------------------------------
// TerrainLinksetsResponder
//---------------------------------------------------------------------------

TerrainLinksetsResponder::TerrainLinksetsResponder(const std::string &pCapabilityURL, LinksetsResponderPtr pLinksetsResponsderPtr)
: mCapabilityURL(pCapabilityURL),
mLinksetsResponsderPtr(pLinksetsResponsderPtr)
{
}

TerrainLinksetsResponder::~TerrainLinksetsResponder()
{
}

void TerrainLinksetsResponder::result(const LLSD &pContent)
{
	mLinksetsResponsderPtr->handleTerrainLinksetsResult(pContent);
}

void TerrainLinksetsResponder::error(U32 pStatus, const std::string &pReason)
{
	mLinksetsResponsderPtr->handleTerrainLinksetsError(pStatus, pReason, mCapabilityURL);
}
