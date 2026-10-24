#ifndef SERVERLOBBY_H
#define SERVERLOBBY_H

#include "address.h"
#include "config.h"
#include "network.h"
#include <atomic>
#include <string>
#include <vector>

namespace ygo {
struct ServerInfo {
	enum Protocol : uint8_t {
		HTTP,
		HTTPS,
	} protocol{ HTTP };
	mutable bool resolved{ false };
	std::wstring name;
	std::string address;
	std::string roomaddress;
	uint16_t duelport;
	uint16_t roomlistport;
	mutable epro::Host resolved_address;
	const epro::Host& Resolved() const;
	epro::stringview GetProtocolString() const {
		return GetProtocolString(protocol);
	}
	static epro::stringview GetProtocolString(Protocol protocol) {
		switch(protocol) {
		case HTTP:
			return "http";
		case HTTPS:
			return "https";
		default:
			unreachable();
		}
	}
	static Protocol GetProtocol(epro::stringview protocol) {
		if(protocol == "https")
			return HTTPS;
		return HTTP;
	}
};
struct RoomInfo {
	std::wstring name;
	uint32_t id;
	bool started;
	bool locked;
	std::vector<std::wstring> players;
	std::wstring description;
	HostInfo info;
};
class ServerLobby {
public:
	static std::vector<RoomInfo> roomsVector;
	static std::vector<ServerInfo> serversVector;
	static bool IsKnownHost(epro::Host host);
	static void RefreshRooms();
	static bool HasRefreshedRooms();
	static void GetRoomsThread();
	static void FillOnlineRooms();
	static void JoinServer(bool host);
	static std::atomic_bool is_refreshing;
	static std::atomic_bool has_refreshed;
	// incremented by the room fetch thread every time a fetch finishes (success or failure)
	static std::atomic<uint32_t> fetch_counter;
	static std::atomic_bool last_fetch_ok;
	// quick match ("Play" button)
	static std::atomic_bool quickMatchActive;         // currently searching
	static std::atomic_bool quickMatchSession;        // a quick match was started and not cancelled by the user
	static std::atomic_bool quickMatchRestartPending; // "Search Next": restart the search when back at the room list
	enum QuickMatchRole { QM_NONE = 0, QM_GUEST = 1, QM_HOST = 2 };
	static std::atomic<int> quickMatchRole;           // role in the room the quick match put us in
	static std::atomic_bool quickMatchTypeChanged;    // the server told us our slot in the room (STOC_TYPE_CHANGE)
	static std::wstring quickMatchStyle;
	static bool quickMatchSearchNext;
	static int quickMatchTimeoutSeconds;
	static bool StartQuickMatch(const std::wstring& style, bool searchNext, int timeoutSeconds);
	static void CancelQuickMatch();
	static void UpdateQuickMatch();
	static void SetQuickMatchUi(bool searching);
	static bool JoinRoomDirect(uint32_t roomid);
	static void HostQuickMatchRoom();
	// defined in menu_handler.cpp (they reuse the room lobby buttons logic)
	static void QuickMatchLobbyTick();
	static void QuickMatchLeaveRoom();
	static std::wstring GetStyleFromBanlistName(const std::wstring& banlistName);
	static std::vector<std::wstring> GetAvailableStyles();
};
//extern ServerLobby serverLobby;
}
#endif //SERVERLOBBY_H
