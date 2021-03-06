#include "stdafx.h"
#include "WOL.h"
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "../main/SQLHelper.h"
#include "../main/RFXtrx.h"
#include "../main/WebServer.h"
#include "../main/mainworker.h"
#include "../webserver/cWebem.h"
#include "../json/json.h"

CWOL::CWOL(const int ID, const std::string &BroadcastAddress, const unsigned short Port):
m_broadcast_address(BroadcastAddress)
{
	m_HwdID=ID;
	m_bSkipReceiveCheck = true;
#ifdef WIN32
	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		_log.Log(LOG_ERROR,"WOL: Error initializing Winsock!");
	}
#endif
	m_wol_port=Port;//9;
}

CWOL::~CWOL(void)
{
	m_bIsStarted=false;
#ifdef WIN32
	WSACleanup();
#endif
}

void CWOL::Init()
{
}

bool CWOL::StartHardware()
{
	Init();
	m_bIsStarted=true;
	sOnConnected(this);

	StartHeartbeatThread();
	_log.Log(LOG_STATUS,"WOL: Started");

	return true;
}

bool CWOL::StopHardware()
{
	StopHeartbeatThread();
	m_bIsStarted=false;
    return true;
}

//6 * 255 or(0xff)
//16 * MAC Address of target PC
bool GenerateWOLPacket(unsigned char *pPacket, const std::string &MACAddress)
{
	std::vector<std::string> results;
	StringSplit(MACAddress, "-", results);
	if (results.size() != 6)
	{
		StringSplit(MACAddress, ":", results);
		if (results.size() != 6)
		{
			return false;
		}
	}

	unsigned char mac[6];
	int ii;

	for (ii = 0; ii < 6; ii++)
	{
		std::stringstream SS(results[ii]);
		unsigned int c;
		SS >> std::hex >> c;
		mac[ii] = (unsigned char)c;
	}

	/** first 6 bytes of 255 **/
	for (ii = 0; ii < 6; ii++) {
		pPacket[ii] = 0xFF;
	}
	/** append it 16 times to packet **/
	for (ii = 1; ii <= 16; ii++) {
		memcpy(&pPacket[ii * 6], &mac, 6 * sizeof(unsigned char));
	}
	return true;
}

bool CWOL::SendWOLPacket(const unsigned char *pPacket)
{
	int udpSocket;
	struct sockaddr_in udpClient, udpServer;

	udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

	// this call is what allows broadcast packets to be sent:
	int broadcast = 1;
	if (setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast)) == -1) 
	{
		return false;
	}
	udpClient.sin_family = AF_INET;
	udpClient.sin_addr.s_addr = INADDR_ANY;
	udpClient.sin_port = 0;

	bind(udpSocket, (struct sockaddr*)&udpClient, sizeof(udpClient));

	/** �make the packet as shown above **/

	/** set server end point (the broadcast addres)**/
	udpServer.sin_family = AF_INET;
	udpServer.sin_addr.s_addr = inet_addr(m_broadcast_address.c_str());
	udpServer.sin_port = htons(m_wol_port);

	/** send the packet **/
	sendto(udpSocket, (const char*)pPacket, 102, 0, (struct sockaddr*)&udpServer, sizeof(udpServer));

	closesocket(udpSocket);

	return true;
}

bool CWOL::WriteToHardware(const char *pdata, const unsigned char length)
{
	tRBUF *pSen=(tRBUF*)pdata;

	unsigned char packettype=pSen->ICMND.packettype;
	//unsigned char subtype=pSen->ICMND.subtype;

	if (packettype!=pTypeLighting2)
		return false;

	if (pSen->LIGHTING2.cmnd != light2_sOn) // only send WOL with ON command
		return true;

	int nodeID=(pSen->LIGHTING2.id3<<8)|pSen->LIGHTING2.id4;

	std::vector<std::vector<std::string> > result;

	//Find our Node
	result=m_sql.safe_query("SELECT MacAddress FROM WOLNodes WHERE (ID==%d)", nodeID);
	if (result.size()<1)
		return false; //Not Found

	unsigned char tosend[102];
	std::string mac_address=result[0][0];
	if (!GenerateWOLPacket(tosend, mac_address))
	{
		_log.Log(LOG_ERROR,"WOL: Error creating magic packet");
		return false;
	}

	if (SendWOLPacket(tosend))
	{
		_log.Log(LOG_STATUS,"WOL: Wake-up send to: %s",mac_address.c_str());
	}
	else
	{
		_log.Log(LOG_ERROR,"WOL: Error sending notification to: %s",mac_address.c_str());
		return false;
	}
	return true;
}

void CWOL::AddNode(const std::string &Name, const std::string &MACAddress)
{
	std::vector<std::vector<std::string> > result;

	//Check if exists
	result=m_sql.safe_query("SELECT ID FROM WOLNodes WHERE (HardwareID==%d) AND (Name=='%q') AND (MacAddress=='%q')",
		m_HwdID, Name.c_str(), MACAddress.c_str());
	if (result.size()>0)
		return; //Already exists
	m_sql.safe_query("INSERT INTO WOLNodes (HardwareID, Name, MacAddress) VALUES (%d,'%q','%q')",
		m_HwdID, Name.c_str(), MACAddress.c_str());

	result=m_sql.safe_query("SELECT ID FROM WOLNodes WHERE (HardwareID==%d) AND (Name=='%q') AND (MacAddress=='%q')",
		m_HwdID, Name.c_str(), MACAddress.c_str());
	if (result.size()<1)
		return;

	int ID=atoi(result[0][0].c_str());

	char szID[40];
	sprintf(szID,"%X%02X%02X%02X", 0, 0, (ID&0xFF00)>>8, ID&0xFF);

	//Also add a light (push) device
	m_sql.safe_query(
		"INSERT INTO DeviceStatus (HardwareID, DeviceID, Unit, Type, SubType, SwitchType, Used, SignalLevel, BatteryLevel, Name, nValue, sValue) "
		"VALUES (%d,'%q',%d,%d,%d,%d,1, 12,255,'%q',1,' ')",
		m_HwdID, szID, int(1), pTypeLighting2, sTypeAC, int(STYPE_PushOn), Name.c_str());
}

bool CWOL::UpdateNode(const int ID, const std::string &Name, const std::string &MACAddress)
{
	std::vector<std::vector<std::string> > result;

	//Check if exists
	result=m_sql.safe_query("SELECT ID FROM WOLNodes WHERE (HardwareID==%d) AND (ID==%d)",
		m_HwdID, ID);
	if (result.size()<1)
		return false; //Not Found!?

	m_sql.safe_query("UPDATE WOLNodes SET Name='%q', MacAddress='%q' WHERE (HardwareID==%d) AND (ID==%d)",
		Name.c_str(), MACAddress.c_str(), m_HwdID, ID);

	char szID[40];
	sprintf(szID,"%X%02X%02X%02X", 0, 0, (ID&0xFF00)>>8, ID&0xFF);

	//Also update Light/Switch
	m_sql.safe_query(
		"UPDATE DeviceStatus SET Name='%q' WHERE (HardwareID==%d) AND (DeviceID=='%q')",
		Name.c_str(), m_HwdID, szID);

	return true;
}

void CWOL::RemoveNode(const int ID)
{
	m_sql.safe_query("DELETE FROM WOLNodes WHERE (HardwareID==%d) AND (ID==%d)",
		m_HwdID, ID);

	//Also delete the switch
	char szID[40];
	sprintf(szID,"%X%02X%02X%02X", 0, 0, (ID&0xFF00)>>8, ID&0xFF);

	m_sql.safe_query("DELETE FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q')",
		m_HwdID, szID);
}

void CWOL::RemoveAllNodes()
{
	m_sql.safe_query("DELETE FROM WOLNodes WHERE (HardwareID==%d)", m_HwdID);

	//Also delete the all switches
	m_sql.safe_query("DELETE FROM DeviceStatus WHERE (HardwareID==%d)",
		m_HwdID);
}

//Webserver helpers
namespace http {
	namespace server {
		void CWebServer::Cmd_WOLGetNodes(Json::Value &root)
		{
			std::string hwid = m_pWebEm->FindValue("idx");
			if (hwid == "")
				return;
			int iHardwareID = atoi(hwid.c_str());
			CDomoticzHardwareBase *pHardware = m_mainworker.GetHardware(iHardwareID);
			if (pHardware == NULL)
				return;
			if (pHardware->HwdType != HTYPE_WOL)
				return;

			root["status"] = "OK";
			root["title"] = "WOLGetNodes";

			std::vector<std::vector<std::string> > result;
			result = m_sql.safe_query("SELECT ID,Name,MacAddress FROM WOLNodes WHERE (HardwareID==%d)",
				iHardwareID);
			if (result.size() > 0)
			{
				std::vector<std::vector<std::string> >::const_iterator itt;
				int ii = 0;
				for (itt = result.begin(); itt != result.end(); ++itt)
				{
					std::vector<std::string> sd = *itt;

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sd[1];
					root["result"][ii]["Mac"] = sd[2];
					ii++;
				}
			}
		}

		void CWebServer::Cmd_WOLAddNode(Json::Value &root)
		{
			if (m_pWebEm->m_actualuser_rights != 2)
			{
				//No admin user, and not allowed to be here
				return;
			}

			std::string hwid = m_pWebEm->FindValue("idx");
			std::string name = m_pWebEm->FindValue("name");
			std::string mac = m_pWebEm->FindValue("mac");
			if (
				(hwid == "") ||
				(name == "") ||
				(mac == "")
				)
				return;
			int iHardwareID = atoi(hwid.c_str());
			CDomoticzHardwareBase *pBaseHardware = m_mainworker.GetHardware(iHardwareID);
			if (pBaseHardware == NULL)
				return;
			if (pBaseHardware->HwdType != HTYPE_WOL)
				return;
			CWOL *pHardware = (CWOL*)pBaseHardware;

			root["status"] = "OK";
			root["title"] = "WOLAddNode";
			pHardware->AddNode(name, mac);
		}

		void CWebServer::Cmd_WOLUpdateNode(Json::Value &root)
		{
			if (m_pWebEm->m_actualuser_rights != 2)
			{
				//No admin user, and not allowed to be here
				return;
			}

			std::string hwid = m_pWebEm->FindValue("idx");
			std::string nodeid = m_pWebEm->FindValue("nodeid");
			std::string name = m_pWebEm->FindValue("name");
			std::string mac = m_pWebEm->FindValue("mac");
			if (
				(hwid == "") ||
				(nodeid == "") ||
				(name == "") ||
				(mac == "")
				)
				return;
			int iHardwareID = atoi(hwid.c_str());
			CDomoticzHardwareBase *pBaseHardware = m_mainworker.GetHardware(iHardwareID);
			if (pBaseHardware == NULL)
				return;
			if (pBaseHardware->HwdType != HTYPE_WOL)
				return;
			CWOL *pHardware = (CWOL*)pBaseHardware;

			int NodeID = atoi(nodeid.c_str());
			root["status"] = "OK";
			root["title"] = "WOLUpdateNode";
			pHardware->UpdateNode(NodeID, name, mac);
		}

		void CWebServer::Cmd_WOLRemoveNode(Json::Value &root)
		{
			if (m_pWebEm->m_actualuser_rights != 2)
			{
				//No admin user, and not allowed to be here
				return;
			}

			std::string hwid = m_pWebEm->FindValue("idx");
			std::string nodeid = m_pWebEm->FindValue("nodeid");
			if (
				(hwid == "") ||
				(nodeid == "")
				)
				return;
			int iHardwareID = atoi(hwid.c_str());
			CDomoticzHardwareBase *pBaseHardware = m_mainworker.GetHardware(iHardwareID);
			if (pBaseHardware == NULL)
				return;
			if (pBaseHardware->HwdType != HTYPE_WOL)
				return;
			CWOL *pHardware = (CWOL*)pBaseHardware;

			int NodeID = atoi(nodeid.c_str());
			root["status"] = "OK";
			root["title"] = "WOLRemoveNode";
			pHardware->RemoveNode(NodeID);
		}

		void CWebServer::Cmd_WOLClearNodes(Json::Value &root)
		{
			if (m_pWebEm->m_actualuser_rights != 2)
			{
				//No admin user, and not allowed to be here
				return;
			}

			std::string hwid = m_pWebEm->FindValue("idx");
			if (hwid == "")
				return;
			int iHardwareID = atoi(hwid.c_str());
			CDomoticzHardwareBase *pBaseHardware = m_mainworker.GetHardware(iHardwareID);
			if (pBaseHardware == NULL)
				return;
			if (pBaseHardware->HwdType != HTYPE_WOL)
				return;
			CWOL *pHardware = (CWOL*)pBaseHardware;

			root["status"] = "OK";
			root["title"] = "WOLClearNodes";
			pHardware->RemoveAllNodes();
		}
	}
}
