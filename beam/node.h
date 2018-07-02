#pragma once

#include "node_processor.h"
#include "../utility/io/timer.h"
#include "../core/proto.h"
#include "../core/block_crypt.h"
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <condition_variable>

namespace beam
{
struct Node
{
	static const uint16_t s_PortDefault = 31744; // whatever

	struct Config
	{
		io::Address m_Listen;
		uint16_t m_BeaconPort = 0; // set to 0 if should use the same port for listen
		uint32_t m_BeaconPeriod_ms = 500;
		std::vector<io::Address> m_Connect;

		std::string m_sPathLocal;
        ECC::NoLeak<ECC::uintBig> m_WalletKey;
		NodeProcessor::Horizon m_Horizon;

		bool m_RestrictMinedReportToOwner = false; // TODO: turn this ON once wallet supports this

		struct Timeout {
			uint32_t m_GetState_ms	= 1000 * 5;
			uint32_t m_GetBlock_ms	= 1000 * 30;
			uint32_t m_GetTx_ms		= 1000 * 5;
			uint32_t m_MiningSoftRestart_ms = 100;
			uint32_t m_TopPeersUpd_ms = 1000 * 60 * 10; // once in 10 minutes
		} m_Timeout;

		uint32_t m_MaxPoolTransactions = 100 * 1000;
		uint32_t m_MiningThreads = 0; // by default disabled
		uint32_t m_MinerID = 0; // used as a seed for miner nonce generation

		// Number of verification threads for CPU-hungry cryptography. Currently used for block validation only.
		// 0: single threaded
		// negative: number of cores minus number of mining threads. 
		int m_VerificationThreads = 0;

		struct HistoryCompression
		{
			std::string m_sPathOutput;
			std::string m_sPathTmp;

			Height m_Threshold = 60 * 24;		// 1 day roughly. Newer blocks should not be aggregated (not mature enough)
			Height m_MinAggregate = 60 * 24;	// how many new blocks should produce new file
			uint32_t m_Naggling = 32;			// combine up to 32 blocks in memory, before involving file system
			uint32_t m_MaxBacklog = 7;
		} m_HistoryCompression;

		struct TestMode {
			// for testing only!
			uint32_t m_FakePowSolveTime_ms = 15 * 1000;

		} m_TestMode;

		std::vector<Block::Body> m_vTreasury;

		Config()
		{
			m_WalletKey.V = ECC::Zero;
		}

	} m_Cfg; // must not be changed after initialization

	~Node();
	void Initialize();
	void ImportMacroblock(Height); // throws on err

	NodeProcessor& get_Processor() { return m_Processor; } // for tests only!

private:

	struct Processor
		:public NodeProcessor
	{
		// NodeProcessor
		virtual void RequestData(const Block::SystemState::ID&, bool bBlock, const PeerID* pPreferredPeer) override;
		virtual void OnPeerInsane(const PeerID&) override;
		virtual void OnNewState() override;
		virtual void OnRolledBack() override;
		virtual bool VerifyBlock(const Block::BodyBase&, TxBase::IReader&&, const HeightRange&) override;

		struct Verifier
		{
			const TxBase* m_pTx;
			TxBase::IReader* m_pR;
			TxBase::Context m_Context;

			bool m_bFail;
			uint32_t m_iTask;
			uint32_t m_Remaining;

			std::mutex m_Mutex;
			std::condition_variable m_TaskNew;
			std::condition_variable m_TaskFinished;

			std::vector<std::thread> m_vThreads;

			void Thread(uint32_t);

			IMPLEMENT_GET_PARENT_OBJ(Processor, m_Verifier)
		} m_Verifier;

		IMPLEMENT_GET_PARENT_OBJ(Node, m_Processor)
	} m_Processor;

	NodeProcessor::TxPool m_TxPool;

	struct Peer;

	struct Task
		:public boost::intrusive::set_base_hook<>
		,public boost::intrusive::list_base_hook<>
	{
		typedef std::pair<Block::SystemState::ID, bool> Key;
		Key m_Key;

		bool m_bRelevant;
		Peer* m_pOwner;

		bool operator < (const Task& t) const { return (m_Key < t.m_Key); }
	};

	typedef boost::intrusive::list<Task> TaskList;
	typedef boost::intrusive::multiset<Task> TaskSet;

	TaskList m_lstTasksUnassigned;
	TaskSet m_setTasks;

	void TryAssignTask(Task&, const PeerID*);
	bool ShouldAssignTask(Task&, Peer&);
	void AssignTask(Task&, Peer&);
	void DeleteUnassignedTask(Task&);

	struct WantedTx
	{
		struct Node
			:public boost::intrusive::set_base_hook<>
			,public boost::intrusive::list_base_hook<>
		{
			Transaction::KeyType m_Key;
			uint32_t m_Advertised_ms;

			bool operator < (const Node& n) const { return (m_Key < n.m_Key); }
		};

		typedef boost::intrusive::list<Node> List;
		typedef boost::intrusive::multiset<Node> Set;

		List m_lst;
		Set m_set;

		void Delete(Node&);

		io::Timer::Ptr m_pTimer;
		void SetTimer();
		void OnTimer();

		IMPLEMENT_GET_PARENT_OBJ(beam::Node, m_Wtx)
	} m_Wtx;

	struct PeerMan
		:public proto::PeerManager
	{
		io::Timer::Ptr m_pTimer;

		struct PeerInfoPlus
			:public PeerInfo
		{
			Peer* m_pLive;
		};

		// PeerManager
		virtual void ActivatePeer(PeerInfo&) override;
		virtual void DeactivatePeer(PeerInfo&) override;
		virtual PeerInfo* AllocPeer() override;
		virtual void DeletePeer(PeerInfo&) override;

		~PeerMan() { Clear(); }

		IMPLEMENT_GET_PARENT_OBJ(Node, m_PeerMan)
	} m_PeerMan;

	struct Peer
		:public proto::NodeConnection
		,public boost::intrusive::list_base_hook<>
	{
		Node* m_pThis;

		PeerMan::PeerInfoPlus* m_pInfo;

		bool m_bConnected;
		bool m_bPiRcvd; // peers should send PeerInfoSelf only once

		beam::io::Address m_RemoteAddr; // for logging only

		Height m_TipHeight;
		proto::Config m_Config;

		TaskList m_lstTasks;
		void TakeTasks();
		void ReleaseTasks();
		void ReleaseTask(Task&);
		void SetTimerWrtFirstTask();

		io::Timer::Ptr m_pTimer;
		io::Timer::Ptr m_pTimerPeers;
		void OnTimer();
		void SetTimer(uint32_t timeout_ms);
		void KillTimer();
		void OnResendPeers();
		void DeleteSelf(bool bIsError, bool bIsBan);
		static void ThrowUnexpected();

		Task& get_FirstTask();
		void OnFirstTaskDone();
		void OnFirstTaskDone(NodeProcessor::DataStatus::Enum);

		std::set<Task::Key> m_setRejected; // data that shouldn't be requested from this peer. Reset after reconnection or on receiving NewTip

		// proto::NodeConnection
		virtual void OnConnected() override;
		virtual void OnClosed(int errorCode) override;
		virtual void get_MyID(ECC::Scalar::Native&) override; // by default no-ID (secure channel, but no authentication)
		virtual void GenerateSChannelNonce(ECC::Scalar&) override; // Must be overridden to support SChannel
		// messages
		virtual void OnMsg(proto::SChannelAuthentication&&) override;
		virtual void OnMsg(proto::Config&&) override;
		virtual void OnMsg(proto::Ping&&) override;
		virtual void OnMsg(proto::NewTip&&) override;
		virtual void OnMsg(proto::DataMissing&&) override;
		virtual void OnMsg(proto::GetHdr&&) override;
		virtual void OnMsg(proto::Hdr&&) override;
		virtual void OnMsg(proto::GetBody&&) override;
		virtual void OnMsg(proto::Body&&) override;
		virtual void OnMsg(proto::NewTransaction&&) override;
		virtual void OnMsg(proto::HaveTransaction&&) override;
		virtual void OnMsg(proto::GetTransaction&&) override;
		virtual void OnMsg(proto::GetMined&&) override;
		virtual void OnMsg(proto::GetProofState&&) override;
		virtual void OnMsg(proto::GetProofKernel&&) override;
		virtual void OnMsg(proto::GetProofUtxo&&) override;
		virtual void OnMsg(proto::PeerInfoSelf&&) override;
		virtual void OnMsg(proto::PeerInfo&&) override;
	};

	typedef boost::intrusive::list<Peer> PeerList;
	PeerList m_lstPeers;

	ECC::NoLeak<ECC::uintBig> m_SChannelSeed;
	PeerID m_MyID;

	Peer* AllocPeer();

	void RefreshCongestions();

	struct Server
		:public proto::NodeConnection::Server
	{
		// NodeConnection::Server
		virtual void OnAccepted(io::TcpStream::Ptr&&, int errorCode) override;

		IMPLEMENT_GET_PARENT_OBJ(Node, m_Server)
	} m_Server;

	struct Beacon
	{
		struct OutCtx;

		uv_udp_t m_Udp;
		bool m_bShouldClose;
		bool m_bRcv;
		OutCtx* m_pOut;
		std::vector<uint8_t> m_BufRcv;

		io::Timer::Ptr m_pTimer;
		void OnTimer();

		Beacon();
		~Beacon();

		void Start();
		uint16_t get_Port();
		void OnPeer(const io::Address&, const PeerID&);

		static void OnRcv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags);
		static void AllocBuf(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);

		IMPLEMENT_GET_PARENT_OBJ(Node, m_Beacon)
	} m_Beacon;

	struct PerThread
	{
		io::Reactor::Ptr m_pReactor;
		io::AsyncEvent::Ptr m_pEvt;
		std::thread m_Thread;
	};

	struct Miner
	{
		std::vector<PerThread> m_vThreads;
		io::AsyncEvent::Ptr m_pEvtMined;

		struct Task
		{
			typedef std::shared_ptr<Task> Ptr;

			// Task is mutable. But modifications are allowed only when holding the mutex.

			Block::SystemState::Full m_Hdr;
			ByteBuffer m_Body;
			Amount m_Fees;

			std::shared_ptr<volatile bool> m_pStop;
		};

		void OnRefresh(uint32_t iIdx);
		void OnMined();

		void HardAbortSafe();
		bool Restart();

		std::mutex m_Mutex;
		Task::Ptr m_pTask; // currently being-mined

		io::Timer::Ptr m_pTimer;
		bool m_bTimerPending;
		void OnTimer();
		void SetTimer(uint32_t timeout_ms, bool bHard);

		IMPLEMENT_GET_PARENT_OBJ(Node, m_Miner)
	} m_Miner;

	struct Compressor
	{
		void Init();
		void OnRolledBack();
		void Cleanup();
		void Delete(const NodeDB::StateID&);
		void OnNewState();
		void FmtPath(Block::BodyBase::RW&, Height, const Height* pH0);
		void StopCurrent();

		void OnNotify();
		void Proceed();
		bool ProceedInternal();
		bool SquashOnce(std::vector<HeightRange>&);
		bool SquashOnce(Block::BodyBase::RW&, Block::BodyBase::RW& rwSrc0, Block::BodyBase::RW& rwSrc1);

		PerThread m_Link;
		std::mutex m_Mutex;
		std::condition_variable m_Cond;

		volatile bool m_bStop;
		bool m_bEnabled;
		bool m_bSuccess;

		// current data exchanged
		HeightRange m_hrNew; // requested range. If min is non-zero - should be merged with previously-generated
		HeightRange m_hrInplaceRequest;

		IMPLEMENT_GET_PARENT_OBJ(Node, m_Compressor)
	} m_Compressor;
};

} // namespace beam
