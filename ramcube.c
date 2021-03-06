#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ctype.h>
#include "time.h"
#include "memcached.h"
#include "hash.h"
#include "ramcube.h"
#include "Log.h"
#include "Segment.h"
#include <sys/timeb.h>
//vpBackupNames and vpBackupOut might change to arrays
// for consistent hash. for that, we should have a fixed name-to-index mapping
//vector<SOCKADDR_IN *> vpBackupSins, vpPingSins;

typedef struct settings settings_t;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct conn conn_t;

#define MAX_COMM_LINE		100

#define BCUBE_RECOVERIES_PER_BACKUP 2
#define MAX_NEIGHBORS 		128

//copied from memcached.c
#define COMMAND_TOKEN		0
#define SUBCOMMAND_TOKEN	1
#define KEY_TOKEN			1
#define MAX_TOKENS			8
//specific for ramcube
#define DATA_CONN_PTR		1
#define PEER_NAME			2
#define VALUE_LENGTH_TOKEN	4

//#define KEY_MAX_LENGTH		32

enum proxy_type {
	PROXY_TYPE_DATA_SERVER,		// data rx
	PROXY_TYPE_PING_CLIENT, 	// ping tx
	PROXY_TYPE_PING_SERVER,		// ping rx
	PROXY_TYPE_BACKUP_CLIENT,	// backup tx
	PROXY_TYPE_BACKUP_SERVER,	// backup rx
	PROXY_TYPE_RECOVERY_CLIENT,
    PROXY_TYPE_RECOVERY_SERVER,
    PROXY_TYPE_COORDINATOR_SERVER, //coordinator
    PROXY_TYPE_COORDINATOR_CLIENT, //coordinator
	PROXY_TYPE_UNKNOWN,
	PROXY_TYPE_NUMBER
};

enum proxy_command_code {
	PROXY_CMD_STATUS,
	PROXY_CMD_SET_TYPE,
	PROXY_CMD_BACKUP_REPLY,
	PROXY_CMD_NEIGHBOR_FAILURE,
	PROXY_CMD_RECOVERY_START,
	PROXY_CMD_RECOVERY_FINISH,
	PROXY_CMD_RECOVERY_REPLY,
	PROXY_CMD_QUIT,
	PROXY_CMD_PING,
	PROXY_CMD_GET,
	PROXY_CMD_SET
};

typedef struct proxy_comand_s {
	enum proxy_command_code m_nCode;
	char *m_lpString;
	unsigned int m_nTokenNumber;
	enum proxy_type m_nProxyType;
} PROXY_COMMAND;

typedef struct ramcube_config_s {
	size_t maxMem, maxSlabSize, minUnitSize;
	unsigned int maxConns;
	//int defaultPort;			//use memcached port for test
	//struct in_addr listenAddr;
	double factor;    
	SOCKADDR_IN me;
	char myAddr[100];
	int myPort;
	unsigned int nRecoveriesPerBackup;

	//we should move the follow to an event_manager class
	int currConns;
	unsigned long succGets, missGets, succSets, missSets;
	unsigned long succSetsBackup, missSetsBackup;
	unsigned long succSetsRecovery, missSetsRecovery;
	unsigned long succSetsRecoveryReturn, missSetsRecoveryReturn, issuedSetsRecovery;

	long long start, totalLatency;

	size_t readBytes, writeBytes, recoveryBytes;
	unsigned long missReadlns;

	char resultFileName[100];
	//int debugToDead;
}ramcube_config_t;

typedef struct neighbor_sin_s {
	SOCKADDR_IN sin;
	bool in_use;
} neighbor_sin_t;

typedef struct ConnProxy_s {
	evutil_socket_t m_fd;	//my fd 
	//client - primary - backup: when I am a backup and processing a backup set,
	//this is used by a backup handling a backup set, and it will be added in BACKUP_REPLY
	void *data_conn_ptr;	

	SOCKADDR_IN m_peerSin;

	//for each pair of (connected) nodes there are two directed connections, 
	//one corresponding to my fd, the other to sibfd
	//in current design, sibfd is valid only when the proxy is actively 
	//initiated (in connect_and_return_ConnProxy()), balabalabala
	//evutil_socket_t sibfd;
	unsigned int m_nMissedPings;

	//char	*currComm;
	char 	*m_currDataPos;  /** when we read in an item's value, it goes here */
	size_t 	m_nLeftData; //init to (real data length + 2) 	(data + \r\n)
//	MemUnit *m_pCurrUnit;     /* temp unit ptr for setting long values  */

	enum proxy_type m_type;

	struct bufferevent *m_bev;
	struct evbuffer *m_inputBuffer;
	struct evbuffer *m_outputBuffer;
	//Now we only use it to store the LOCAL set cmd str.
	//TODO: in the future we might let it point to cmd str from evbuffer_readln and delete when we won't use it
	char m_currComm[MAX_COMM_LINE];	//TODO: this can be optimized 
	//char *currCommPos;

	//valid when type == PROXY_TYPE_BACKUP_IN or PROXY_TYPE_RECOVERY_OUT or PROXY_TYPE_RECOVERY_IN
//	CacheChain *m_pBackupChain;		
	//SOCKADDR_IN m_deadSin;	//valid for type == PROXY_TYPE_RECOVERY_IN

	//ONLY valid when type == PROXY_TYPE_RECOVERY_IN. Simply assume tha last one is for current recovery, this is to receive (NOT transmit) recovery data!!!
	//std::vector<CacheChain *> vpRecoveryChains;  

	bool m_isInRecovery;

//	static BackupService m_cBackupService;
//	static RecoveryService m_cRecoveryService;

//	PROXY_COMMAND m_sProxyCommandTable[];

//	DataSegment *m_pReceivedRecoverSegment;
	int m_iTotalRecoverSegmentRead;
//	DWORD m_dwRecoverBeginTime;

	conn_t *connection;

    //for heartbeart
    struct event evt;

    char sendInfo[64];
    int outTimes;
    //SOCKADDR_IN m_sin;
}ConnProxy;

typedef struct {
    char *value;
    size_t length;
} ramcube_token_t;

/*typedef struct {
	evutil_socket_t sfd;
    conn_t *c;
} socket_conn_pair;
socket_conn_pair mapWaitBackup[MAX_NEIGHBORS];

conn_t *gWaitConn[MAX_NEIGHBORS];
*/
//memcached uses this to decide whether to support ramcube
char *ramcube_config_file; 

ramcube_config_t config;
neighbor_sin_t neighbors[MAX_NEIGHBORS];
SOCKADDR_IN *vpBackupSins[MAX_NEIGHBORS], *vpPingSins[MAX_NEIGHBORS];


//NOTE: vpBackupOut contains only backup connections initiated by me, 
//i.e., not including connections initiated by my peers.
ConnProxy *vpBackupOut[MAX_NEIGHBORS], *vpPingOut[MAX_NEIGHBORS];
//conn in vpBackupIn is constructed after accepting peer's conn request
ConnProxy *vpBackupIn[MAX_NEIGHBORS], *vpPingIn[MAX_NEIGHBORS]; 
ConnProxy *vpRecoveryOut[MAX_NEIGHBORS], *vpRecoveryIn[MAX_NEIGHBORS];

//增加一个和coordinator通信的类型结构，
SOCKADDR_IN *vpCoordinatorSins[MAX_NEIGHBORS];
ConnProxy *vpCoordinatorOut[MAX_NEIGHBORS], *vpCoordinatorIn[MAX_NEIGHBORS];



int init_smaller_proxies(struct event_base *, enum proxy_type);
void init_ramcube_config(const settings_t *);
int set_sin_by_name(char *, SOCKADDR_IN *);
SOCKADDR_IN *alloc_sin_from_neighbors(void);
int push_back_sin(SOCKADDR_IN **, SOCKADDR_IN *);
int push_back_proxy(ConnProxy **, ConnProxy *);
int inet_addr_compare(char *, int, char *, int);
ConnProxy *connect_and_return_ConnProxy(struct event_base *, 
		const SOCKADDR_IN *, enum proxy_type);
void read_cb(struct bufferevent *, void *);
void write_cb(struct bufferevent *, void *);
void event_cb(struct bufferevent *, short, void *);

int init_ConnProxy(struct bufferevent *, enum proxy_type, const SOCKADDR_IN*, ConnProxy *);
int process_command_TYPE(ramcube_token_t *, ConnProxy *);
ConnProxy *get_backup_proxy_by_key(const char *, size_t);
void send_unit_to_backup(ConnProxy *, item *, void *);
int process_command_BACKUP_REPLY(ramcube_token_t *, ConnProxy *);
void send_string_via_out_cp(ConnProxy *, const char *);
static size_t ramcube_tokenize_command(char *command, ramcube_token_t *tokens, const size_t max_tokens);

//++++++++++++++++++++++++++++++++++ recovery ++++++++++++++++++++++++++++++++++++++//
void send_to_recovery(char *ip, int port, ConnProxy *cp);
void segmentToString(Segment *seg, char *str);
//客户端发送心跳包
void HeartBeat(ConnProxy *cp, struct event_base *pBase);
static void HB_cb(int fd, short ev, void *arg);
int sendHeartBeat(ConnProxy * cp);
char localname[16]; //record the type of me
void Recovery(ConnProxy * cp);

void autoAddTimes(ConnProxy *cp, struct event_base *pBase);
static void recovery_cb(int fd, short event, void *arg);
//record brokenMaster and correspongding recovery
ConnProxy *recoveryBackupConnect(char *Ip, int port, struct event_base *pBase);
void send_request_to_backup(ConnProxy *cp);

#define MAX_MSG_BUFFER	3
#define MAX_HEARTBEAT_TIMES 3
typedef struct brokenMaster {
    char b_master_ip[16];
    int b_master_port;
    char b_backup_ip[16];
    int b_backup_port;
    char b_recovery_ip[16];
    int b_recovery_port;
    bool used;
} BrokenMaster;

/*
 * recovery接收到primary的信息后存储相关信息
 * primary_ip和port用于记录是哪一个primary的心跳信号
 * used表示这个结构体是否处于使用中
 * MaxTimes是最大丢包次数
 */
typedef struct heartBeatInfo {
    // 一次最大链接的PRIMARY数目
    #define MAX_PRIMARY 5

    char b_primary_ip[16];
    int b_primary_port;
    int MaxTimes;
    bool used;
} HeartBeatInfo;

HeartBeatInfo PrimaryTable[MAX_PRIMARY]; // 定义全局接受Primary HeartBeat的结构体

BrokenMaster bokenMasterTable[MAX_MSG_BUFFER]; //it's enough to store infomation, when coordinator send the data, change used flag equal flase
static long long getSystemTime() {
    struct timeb t;
    ftime(&t);
    return 1000 * t.time + t.millitm;
}

void ramcube_adjust_memcached_settings(settings_t *s)
{	
	assert(ramcube_config_file);
	s->num_threads = 1; // ramcube prefer to single-threaded model
	char str[100];
	printf("#num_threads# changed to %d\n", s->num_threads);

	if(ramcube_config_file[0] == 0) {
		printf("ramcube not initiated. this is a pure memcached\n\n");
		ramcube_config_file = NULL;
		return;
	}
		//ramcube_config_file = "config_default.txt";

	FILE* f = freopen(ramcube_config_file,"r",stdin);
	assert(f != NULL);
	
    // just read the first line 
	while (fscanf(f, "%s",str)!=EOF) {
        if (strcmp(str, "me") == 0){
            strcpy(localname, str);
			char name[100];
			char *cport;
			unsigned int iport;
			printf("++++++%s++++++++%s\n",ramcube_config_file, str);
            if (fscanf(f, "%s", name) == 1) {
                //char delim = '#';
			    printf("++++++++++++++%s\n",str);
                cport = strstr(name, "#");              			
                assert(cport != NULL);                    			
                cport++;                                  			
                iport = atoi(cport);                      			
                s->port = iport;                          			
                printf("#port# changed to %d\n", s->port);			
            }
            else {
                printf("fail to read!\n");
            }
            
        } else if (strcmp(str, "coordinator") == 0){
            strcpy(localname, str);
            char name[100];
            char *cport;
            unsigned int iport;
            printf("++++++%s++++++++%s\n",ramcube_config_file, str);
            if (fscanf(f, "%s", name) == 1) {
                //char delim = '#';
                printf("++++++++++++++%s\n",str);
                cport = strstr(name, "#");
                assert(cport != NULL);
                cport++;
                iport = atoi(cport);
                s->port = iport;
                printf("#port# changed to %d\n", s->port);
            }
            else {
                printf("fail to read!\n");
            }

        } else if (strcmp(str, "primary") == 0){
            strcpy(localname, str);
            char name[100];
            char *cport;
            unsigned int iport;
            printf("++++++%s++++++++%s\n",ramcube_config_file, str);
            if (fscanf(f, "%s", name) == 1) {
                //char delim = '#';
                printf("++++++++++++++%s\n",str);
                cport = strstr(name, "#");
                assert(cport != NULL);
                cport++;
                iport = atoi(cport);
                s->port = iport;
                printf("#port# changed to %d\n", s->port);
            }
            else {
                printf("fail to read!\n");
            }

        }
    }
}

void ramcube_init(struct event_base *main_base, const settings_t *s)
{
	assert(ramcube_config_file);

//	printf("sizeof(neighbors): %lu, sizeof(vpBackupSins): %lu\n", 
//			sizeof(neighbors), sizeof(vpBackupSins));
	memset(neighbors, 0, sizeof(neighbors));
	memset(vpBackupSins, 0, sizeof(vpBackupSins));
	memset(vpPingSins, 0, sizeof(vpPingSins));
    memset(vpCoordinatorSins, 0, sizeof(vpCoordinatorSins));

	memset(vpBackupOut, 0, sizeof(vpBackupOut));
	memset(vpPingOut, 0, sizeof(vpPingOut));
	memset(vpBackupIn, 0, sizeof(vpBackupIn));
	memset(vpPingIn, 0, sizeof(vpPingIn));
	memset(vpRecoveryOut, 0, sizeof(vpRecoveryOut));
	memset(vpRecoveryIn, 0, sizeof(vpRecoveryIn));

    memset(vpCoordinatorOut, 0, sizeof(vpCoordinatorOut));
    memset(vpCoordinatorIn, 0, sizeof(vpCoordinatorIn));


	//memset(gWaitConn, 0, sizeof(gWaitConn));

    //! add by Aaron on 1th April 2015
    init_object();
    init_SegmentManager();

	init_ramcube_config(s);
	init_smaller_proxies(main_base, PROXY_TYPE_PING_CLIENT);
    init_smaller_proxies(main_base, PROXY_TYPE_BACKUP_CLIENT);
    init_smaller_proxies(main_base, PROXY_TYPE_COORDINATOR_CLIENT);
    //recoveryBackupConnect("127.0.0.1", 11121, main_base);

}

void init_ramcube_config(const settings_t *settings) 
{
	char name[100], s[100];
    //int rtn;
	
	config.nRecoveriesPerBackup = BCUBE_RECOVERIES_PER_BACKUP;
	strcpy(config.resultFileName, "result_default.txt");
	
	//read from config file
	FILE* f = freopen(ramcube_config_file,"r",stdin);
	assert(f != NULL);
	printf("Reading parameters from %s.\n", ramcube_config_file);

	//config.listenAddr.s_addr = htonl(INADDR_ANY); 	// by default listen to all addrs

	while (fscanf(f, "%s",s)!=EOF)
	{
		printf("reading #%s#\n", s);

		if (strcmp(s,"result_file") == 0){
            if (fscanf(f, "%s",config.resultFileName) == 1) {
                //printf("success to read result_file!\n\n");
            }
            else {
                //printf("fail to read!\n");
            }
		}
		if (strcmp(s,"nRecoveriesPerBackup") == 0){
            if (fscanf(f, "%u",&config.nRecoveriesPerBackup) == 1) {
                //printf("success to read nRecoveriesPerBackup!\n\n");
            }
            else {
                //printf("fail to read!\n");
            }
        }
        if (strcmp(s, "me") == 0 || strcmp(s, "coordinator") == 0 || strcmp(s, "primary") == 0){
            if (fscanf(f, "%s", name) == 1) {
                //printf("success to read me!\n\n");
            }
            else {
                //printf("fail to read!\n");
            }
            //rtn =
            set_sin_by_name(name, &config.me);
            //assert(rtn == 0);
		}
		if (strcmp(s,"backups") == 0){
			char tmp[100];
            if (fscanf(f, "%s", tmp) == 1) {
                //printf("success to read backups!\n\n");
            }
            else {
                //printf("fail to read!\n");
            }
			SOCKADDR_IN *pSin = alloc_sin_from_neighbors();
			assert(pSin != NULL);
			memset(pSin, 0, sizeof(*pSin));
            //rtn =
            set_sin_by_name(tmp, pSin);
            //assert(rtn == 0);
			push_back_sin(vpBackupSins, pSin);
		}
        if (strcmp(s,"pings") == 0){
			char tmp[100];
            if (fscanf(f, "%s", tmp) == 1) {
                printf("success to read ping!\n\n");
            }
            else {
                printf("fail to read!\n");
            }
			SOCKADDR_IN *pSin = alloc_sin_from_neighbors();
			assert(pSin != NULL);
			memset(pSin, 0, sizeof(*pSin));
            //rtn =
            set_sin_by_name(tmp, pSin);
            //assert(rtn == 0);
			push_back_sin(vpPingSins, pSin);
        }

        if (strcmp(s,"coordinators") == 0){
            char tmp[100];
            if (fscanf(f, "%s", tmp) == 1) {
                printf("success to read coordinators!\n\n");
            }
            else {
                printf("fail to read!\n");
            }
            SOCKADDR_IN *pSin = alloc_sin_from_neighbors();
            assert(pSin != NULL);
            memset(pSin, 0, sizeof(*pSin));
            //rtn =
            set_sin_by_name(tmp, pSin);
            //assert(rtn == 0);
            push_back_sin(vpCoordinatorSins, pSin);
        }

	} //end of while (fscanf(f, "%s",s)!=EOF)

	strcpy(config.myAddr, inet_ntoa(config.me.sin_addr));
	config.myPort = ntohs(config.me.sin_port);
	//printf("I am %s:%d\n", config.myAddr, config.myPort);

}

//TODO
/* Here we only init smaller backups. Bigger ones are inited after accepting corresponding conn requests*/
int init_smaller_proxies(struct event_base *pBase, enum proxy_type type)
{

	//vector<SOCKADDR_IN *> *pvpSin = NULL;
	SOCKADDR_IN **pvpSin = NULL;
	
    char *strBackup = "BACKUP", *strPing = "PING", *strCoordinator = "COORDINATOR";
	char *typeStr = NULL;

	if (type == PROXY_TYPE_BACKUP_CLIENT) {
        /* 心跳 */
        //HeartBeat("129", 123, pBase);
		pvpSin = vpBackupSins;
		typeStr = strBackup;
	} 
	else if (type == PROXY_TYPE_PING_CLIENT){
		pvpSin = vpPingSins;
		typeStr = strPing;
    }
    else if (type == PROXY_TYPE_COORDINATOR_CLIENT){
        pvpSin = vpCoordinatorSins;
        typeStr = strCoordinator;
    }
	else{
		printf ("ERROR: wrong proxy type (%d)\n", type);
		return -1;
	}

	//vector<SOCKADDR_IN *>::iterator sinIter;
	//printf("pvpSin size: %d\n", pvpSin->size());
	int i;
    //!TODO here need to optimize MAX_NEIGHBORS=128, it's a waste of time to full loop 
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		SOCKADDR_IN *sin = pvpSin[i];
		if (sin == NULL)
			break;
		
		char name[100];
		
		strcpy(name, inet_ntoa(sin->sin_addr));
		int port = ntohs(sin->sin_port);
		//printf("%s (%s:%d), me (%s:%d)\n", typeStr, name, port, myAddr, myPort);

//		struct in_addr sMyAddr = {0}, sPeerAddr = {0};
//		inet_aton(config.myAddr, &sMyAddr);
//		inet_aton(name, &sPeerAddr);

//		int iMyAddr = sMyAddr.S_un.S_un_b.s_b4;
//		int iPeerAddr = sPeerAddr.S_un.S_un_b.s_b4;

		int iCompareResult = inet_addr_compare(config.myAddr, config.myPort, name, port);

		//_strlwr_s(name, strlen(name)+1);
		if (iCompareResult > 0)	{
			printf("Let me connect to my %s (%s:%d).\n", typeStr, name, port);
			//needInit = true;
		} else if (iCompareResult < 0) {
			printf("%s peer (%s:%d) is greater than me (%s:%d), so " 
					"I won't connect to it until it connects to me.\n", 
					typeStr, name, port, config.myAddr, config.myPort);
			//needInit = false;
			continue;
		} else {
			printf("ERROR: %s peer (%s:%d) is the same as me (%s:%d).\n", typeStr, name, port, config.myAddr, config.myPort);
			continue;
		}

		/*ConnProxy *cp =*/ connect_and_return_ConnProxy(pBase, sin, type);
		//assert(cp);
	}

	return 0;
}


//name: wng-snet36#11111, 192.168.4.37#11111
int set_sin_by_name(char *name, SOCKADDR_IN *pSin)
{
    char *cport;//, *cname;
	unsigned int iport;
	
	assert(name != NULL);
	//char delim = '#';
    //cname =
    strtok(name, "#");
    //assert(cname != NULL);
	cport = name + strlen(name) + 1;
	iport = atoi(cport);
	
	struct hostent *host = NULL;
	struct in_addr addr;

	//printf("name#port: %s#%d\n", name, iport);
	if (isalpha(name[0])) {        /* host address is a name */
		host = gethostbyname(name);		
		memcpy(&pSin->sin_addr, host->h_addr, sizeof(struct in_addr));
		pSin->sin_port = htons(iport);
		pSin->sin_family = AF_INET;// AF_INET default 2
	} 
	else {
		addr.s_addr = inet_addr(name);
//		if (addr.s_addr != INADDR_NONE) {
//			remoteHost = gethostbyaddr((char *) &addr, 4, AF_INET);
//		}
		
		memcpy(&pSin->sin_addr, &(addr.s_addr), sizeof(struct in_addr));
		pSin->sin_port = htons(iport);
		pSin->sin_family = AF_INET;
	}
	
	printf("sin (%s:%d) is set by name (%s#%d)\n", inet_ntoa(pSin->sin_addr), 
			ntohs(pSin->sin_port), name, iport);
	return 0;

}

SOCKADDR_IN *alloc_sin_from_neighbors(void)
{
	int i;
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		if (neighbors[i].in_use == false) {
			neighbors[i].in_use = true;
			return &(neighbors[i].sin);
		}
	}

	return NULL;
}

int push_back_sin(SOCKADDR_IN **sin_array, SOCKADDR_IN *new_sin)
{
	int i;
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		if (sin_array[i] == NULL) {
			sin_array[i] = new_sin;
			return 0;
		}
	}

	return -1;
}

int push_back_proxy(ConnProxy **Proxies, ConnProxy *cp)
{
	int i;
	
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		if (Proxies[i] == NULL) {
			Proxies[i] = cp;
			return 0;
		}
	}

	return -1;
}

int inet_addr_compare(char *lpAddr1, int iPort1, char *lpAddr2, int iPort2)
{
	struct in_addr Addr1 = {0}, Addr2 = {0};
	inet_aton(lpAddr1, &Addr1);
	inet_aton(lpAddr2, &Addr2);

	int iAddr1, iAddr2;
//	int iAddr1 = sAddr1.S_un.S_un_b.s_b4;
//	int iAddr2 = sAddr2.S_un.S_un_b.s_b4;
	iAddr1 = Addr1.s_addr;
	iAddr2 = Addr2.s_addr;

	if(iAddr1 < iAddr2)
		return -1;
	else if(iAddr1 > iAddr2)
		return 1;
	else
	{
		if(iPort1 < iPort2)
			return -1;
		else if(iPort1 > iPort2)
			return 1;
	}

	return 0;
}

ConnProxy *connect_and_return_ConnProxy(struct event_base *pBase, 
		const SOCKADDR_IN *pSin, enum proxy_type type)
{
	ConnProxy **pvpProxies = NULL;
	ConnProxy *cp;
    char *strBackup = "BACKUP", *strPing = "PING", *strRecovery = "RECOVERY", *strCoordinator = "COORDINATOR";
	char *typeStr = NULL;
	int rtn;

	if (type == PROXY_TYPE_BACKUP_CLIENT) {
		pvpProxies = vpBackupOut;
		typeStr = strBackup;
	} 
	else if (type == PROXY_TYPE_PING_CLIENT){
		pvpProxies = vpPingOut;
		typeStr = strPing;
	} 
	else if (type == PROXY_TYPE_RECOVERY_CLIENT){
		pvpProxies = vpRecoveryOut;
		typeStr = strRecovery;
    }
    else if (type == PROXY_TYPE_COORDINATOR_CLIENT){
        pvpProxies = vpCoordinatorOut;
        typeStr = strCoordinator;
    }
	else{
		printf ("ERROR: wrong proxy type (%d)\n", type);
		return NULL;
	}

	cp = (ConnProxy *)calloc(1, sizeof(ConnProxy));
	assert(cp != NULL);
	
	//connect to peer
	struct bufferevent *bev = bufferevent_socket_new(pBase, -1, BEV_OPT_CLOSE_ON_FREE);
    if (bev == NULL) {
        fprintf(stderr,"error to new socket bufferevent\n");
    }
	//bufferevent_setwatermark(bev, EV_READ, 0, DATA_BUFFER_SIZE);
    bufferevent_setcb(bev, read_cb, NULL, event_cb, (void *)cp);

	//we encode addr and port in TYPE command since we want to identify a peer by port when debugging on one machine. 
	//in practice, we can use getpeername() to achive that

	rtn = bufferevent_socket_connect(bev, (struct sockaddr *)pSin, sizeof(*pSin));
	if ( rtn< 0) {
		/* Error starting connection */
		//bufferevent_free(bev);	
		perror("bufferevent_socket_connect");
		printf("ERROR: bufferevent_socket_connect (to %s:%d) failed\n", 
				inet_ntoa(pSin->sin_addr), ntohs(pSin->sin_port));
		return NULL;
	}

	bufferevent_enable(bev, EV_READ|EV_PERSIST);
	init_ConnProxy(bev, type, pSin, cp);
	rtn = push_back_proxy(pvpProxies, cp);
	assert(!rtn);
	
	char tmpStr[100];
	evutil_snprintf(tmpStr, 100, "TYPE %s %s#%d\r\n", typeStr, 
			config.myAddr, config.myPort);
	printf("sending str: %s\n", tmpStr);
	bufferevent_write(bev, tmpStr, strlen(tmpStr));

    //if (type == PROXY_TYPE_RECOVERY_CLIENT) { // direct to write data to recovery
        //send_to_recovery(cp);
    //}
//	pvpProxies->push_back(cp);
	printf("connect_and_return_ConnProxy(): (%s:%d) added to %s\n", 
			inet_ntoa(pSin->sin_addr), ntohs(pSin->sin_port), 
			(type == PROXY_TYPE_BACKUP_CLIENT ? 
				"vpBackupOut" : 
                (type == PROXY_TYPE_PING_CLIENT ?
                     "vpPingOut" :
                (type == PROXY_TYPE_COORDINATOR_CLIENT ? "vpCoordinatorOut" : "vpRecoveryOut"))));


    //if (strcmp(localname, "coordinator") == 0){
           /* 心跳机制 */
      //     HeartBeat(cp, pBase);
    //}

    /*
     * primary第一步
     * primary在接到链接请求后，如果自己是primary就发送心跳信号。
     * 并不是所有的backup和ping我都启动心跳，只对ping启动心跳
     */
    if (strcmp(localname, "primary") == 0 && type == PROXY_TYPE_PING_CLIENT) {
        HeartBeat(cp, pBase);
    }
	return cp;
}

void read_cb(struct bufferevent *bev, void *ctx)
{
	ConnProxy *cp = (ConnProxy *)ctx;
	assert(cp);
	//we have 2 kinds of proxies. 'out' proxy is initiated by me and have no idea of the 
	//connection; 'in' proxy is built after memcached receiving TYPE request and thus 
	//know its corres conn. read_cb() is registered only for 'out' proxy, thus c should 
	//be NULL
	assert(cp->connection == NULL);

	size_t nBuf;
	char currComm[MAX_COMM_LINE];
    int flg = 0;
    char str[1024 * 10] = "";
	while ((nBuf = evbuffer_get_length(cp->m_inputBuffer)) > 0) {
		size_t nReadln = 0;
		char *tmp = evbuffer_readln(cp->m_inputBuffer, &nReadln, EVBUFFER_EOL_CRLF_STRICT);
		if (nReadln == 0)	// Note that sometimes libevent may deliver one line ("*\r\n") in several read callbacks.
			break;
        if (flg == 0) {
            assert(nReadln <= MAX_COMM_LINE);
            evutil_snprintf(currComm, MAX_COMM_LINE, "%s", tmp);
            free(tmp);

            ramcube_token_t tokens[MAX_TOKENS];
            size_t ntokens;
            ntokens = ramcube_tokenize_command(currComm, tokens, MAX_TOKENS);

            if (ntokens == 3 && strcmp(currComm, "BACKUP_REPLY") == 0
                    && cp->m_type == PROXY_TYPE_BACKUP_CLIENT) {
                process_command_BACKUP_REPLY(tokens, cp);
            } else if (ntokens == 2 && strcmp(currComm, "ALIVE") == 0) { //heartbeat
                printf("->->->->-> : receive \"ALIVE\" frome %s : %d\n", inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
                --(cp->outTimes);
            }
            // 第14***步 recovery 收到信息
            else if (ntokens == 2 && strcmp(currComm, "DATA_REPLAY") == 0) {
                printf("->->->->-> : receive \"DATA_REPLAY\" from %s : %d\n", inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
                flg = 1;
            }
        } else { //第15****步 处理收到的recovery信息
            strcat(str, tmp);
            strcat(str, "\r\n");
            free(tmp);
            printf("%s\n", str);
        }
	}

    // 第15***步 处理收到的recovery信息
    if (flg == 1) {

    }



	//FOR SPEED TEST ONLY!
	//evbuffer_drain(p->input, DATA_BUFFER_SIZE);	
	//p->send_string("STORED");

//	read_proxy(p);
}

void write_cb(struct bufferevent *bev, void *ctx)
{
    ConnProxy *cp = (ConnProxy *)ctx;
    assert(cp);

    size_t nBuf = evbuffer_get_length(cp->m_outputBuffer);
    fprintf(stderr,"++++++++++%lu\n", nBuf);

	return;
//	ConnProxy *p = (ConnProxy *)ctx;

//	if (p->m_isInRecovery) {
//		//p->add_recovery_data_to_buf(WRITE_POOL_SIZE);
//	}
}

void event_cb(struct bufferevent *bev, short events, void *ctx)
{
	//ConnProxy *cp = (ConnProxy *)ctx;
	//assert(cp);
}

int init_ConnProxy(struct bufferevent *bufev, enum proxy_type t, 
	const SOCKADDR_IN *pSin, ConnProxy *cp)
{
	//assert(fd == 0);
	cp->m_bev = bufev;
	cp->m_fd = bufferevent_getfd(bufev);
	cp->m_inputBuffer = bufferevent_get_input(bufev); 
	cp->m_outputBuffer = bufferevent_get_output(bufev);
	cp->m_type = t;
    cp->outTimes = 0; //heartbeat times
    //cp->m_sin = config.me;
	//for debugging on one node 
	//pSin == NULL means this init is called by accept(). peerSin will be set 
	//in a later process_command_TYPE()
	if (pSin){ 
		memcpy(&cp->m_peerSin, pSin, sizeof(SOCKADDR_IN));
	}

	return 0;	//succeed
}

ulong ramcube_process_commands(conn *c, void *t, const size_t ntokens, char *left_com)
{

    /* add recovery modle */
    //recoveryBackupConnect("127.0.0.1", 11121, c->thread->base);

	assert(ramcube_config_file);
	
	ramcube_token_t *tokens = (ramcube_token_t *)t;
	assert(tokens);

	ConnProxy *cp = (ConnProxy *)(c->ramcube_proxy);
	char *comm = tokens[COMMAND_TOKEN].value;
	assert(comm);
    printf("in ramcube_process_commands: %s+++++++++++++++++\n",comm);

	if (ntokens == 4 && strcmp(comm, "TYPE") == 0 && cp->m_type == PROXY_TYPE_DATA_SERVER) {
		process_command_TYPE(tokens, cp);
		return 0;
	}
	else if (ntokens == 3 && strcmp(comm, "BACKUP_REQUEST") == 0 
			&& cp->m_type == PROXY_TYPE_BACKUP_SERVER) {
		//prepare for handling backup 'set'
        printf("backup receive data:%s\n\n", left_com);
        cp->data_conn_ptr = (void *)atol(tokens[DATA_CONN_PTR].value); //DATA_CONN_PTR=1
		assert(cp->data_conn_ptr);

        //! add by Aaron on 1th April 2015
        appendToSegment(left_com); //receive data and store to memory
        printf("++++++++1212++++++\n\n");
        //! we have store the data to memory, next we will send a reply to server
        char str[100];
        snprintf(str, 100, "BACKUP_REPLY %lu", (ulong)cp->data_conn_ptr);
        out_string(c, str);

        //! test!!!!!!!!!!!!
        //ConnProxy *r_cp = recoveryBackupConnect("127.0.0.1", 11121, cp->connection->thread->base);
        //send_to_recovery("127.0.0.1", 11111, r_cp);
        //! test over!!!!!!!!!!!

        return 1;  //! useful
	} 
	/*"BACKUP_REPLY" is intercepted by read_cb, so the following is useless*/
	else if (ntokens == 3 && strcmp(comm, "BACKUP_REPLY") == 0
			&& cp->m_type == PROXY_TYPE_BACKUP_CLIENT) {
		process_command_BACKUP_REPLY(tokens, cp);
		return 0;
	}	
    // 第15步 recovery接收到backup发来的数据，进行处理
    else if (ntokens == 3 && strcmp(comm, "RECOVERY_REQUEST") == 0
             && cp->m_type == PROXY_TYPE_RECOVERY_SERVER) {
        long long start=getSystemTime();//test
        printf("+++++++++start time: %lld ms\n", start);//test
        int len = strlen(left_com);
        printf("#########> receive recovery backup data : %d, process... ...\n", len);

        //! add by Aaron on 1th April 2015
        //appendToSegment(left_com); //receive data and store to memory

        //! we have store the data to memory, next we will send a reply to server
        //char str[100];
        //snprintf(str, 100, "BACKUP_REPLY %lu", (ulong)cp->data_conn_ptr);
        //out_string(c, str);
        //return 1;  //! useful
        return 2;
    }
    /*
     * recovery第五步
     * 接收到”HEARTBEAT"信息，进行处理
     */
    else if (ntokens == 2 && strcmp(comm, "HEARTBEAT") == 0) {
        printf("->->->->-> : receive \"HEARTBEAT\" frome %s : %d\n", inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
        int i;
        for (i = 0; i < MAX_PRIMARY; ++i) {
            // 已经在Primary循环体中
            if (strcmp(PrimaryTable[i].b_primary_ip, inet_ntoa(cp->m_peerSin.sin_addr)) == 0
                       && PrimaryTable[i].b_primary_port == ntohs(cp->m_peerSin.sin_port)) {
                --PrimaryTable[i].MaxTimes;
                printf("MaxTimes: %d\n", PrimaryTable[i].MaxTimes);
                break;
            } else if (PrimaryTable[i].used == false) { // 找到一个没有使用的PrimaryTable空间
                PrimaryTable[i].used = true;
                strcpy(PrimaryTable[i].b_primary_ip, inet_ntoa(cp->m_peerSin.sin_addr));
                PrimaryTable[i].b_primary_port = ntohs(cp->m_peerSin.sin_port);
                autoAddTimes(cp, cp->connection->thread->base);
                break;
            }
        }

        return 1;
    }
    // 第9步 coordinator收到recovery信息
    else if (ntokens == 2 && strcmp(comm, "CRASH") == 0) {
        char b_primary_ip[16] = "";
        int b_primary_port = 0;
        char b_recovery_ip[16] = "";
        int b_recovery_port = 0;
        char *delim = "\r\n :";
        char *p;
        strcpy(b_primary_ip, strtok(left_com, delim));
        while ((p = strtok(NULL, delim))) {
            b_primary_port = atoi(p);
        }
        strcpy(b_recovery_ip, inet_ntoa(cp->m_peerSin.sin_addr));
        b_recovery_port = ntohs(cp->m_peerSin.sin_port);
        printf("*>*>*>*>*> : receive \"CRASH:%s:%d\"\n", b_primary_ip, b_primary_port);

        // 第10步 找到primary对应的backup,暂时地址是手动设置
        char backupIp[16] = "10.107.19.9";
        int backupPort = 11114;
        int i;
        for (i = 0; i < MAX_NEIGHBORS; i++) {
            if (vpCoordinatorOut[i] == NULL) {
                break;
            } else if (strcmp(inet_ntoa(vpCoordinatorOut[i]->m_peerSin.sin_addr), backupIp) == 0
                && backupPort == ntohs(vpCoordinatorOut[i]->m_peerSin.sin_port)) {

                // 第11步 找到相应的backup，向backup发送信息
                // 为了简单处理，前面的IP:PORT是primary信息，后面的IP:PORT是recovery的信息
                evbuffer_add_printf(vpCoordinatorOut[i]->m_outputBuffer, "CRASHED\n\r%s:%d %s:%d",
                                    b_primary_ip, b_primary_port, b_recovery_ip, b_recovery_port);
                return 1;

            }


        }

        //ConnProxy *r_cp = recoveryBackupConnect(b_recovery_ip, b_recovery_port, cp->connection->thread->base);

        //send_to_recovery(b_primary_ip, b_primary_port, r_cp);

        return 1;
    }
    // 第12步 backup收到coordinator信息
    else if (ntokens == 2 && strcmp(comm, "CRASHED") == 0) {
        char b_primary_ip[16] = "";
        int b_primary_port = 0;
        char b_recovery_ip[16] = "";
        int b_recovery_port = 0;
        char *delim = "\r\n :";
        char *p;
        strcpy(b_primary_ip, strtok(left_com, delim));
        while ((p = strtok(NULL, delim))) {
            if (b_primary_port == 0) {
                b_primary_port = atoi(p);
            } else if (strcmp(b_recovery_ip, "") == 0) {
                strcpy(b_recovery_ip, p);
            } else if (b_recovery_port == 0) {
                b_recovery_port = atoi(p);
            }
        }
        printf("*>*>*>*>*> : receive \"CRASH:%s:%d RECOVERY:%s:%d\"\n",
               b_primary_ip, b_primary_port, b_recovery_ip, b_recovery_port);

        // 第13步 连接recovery
        ConnProxy *r_cp = recoveryBackupConnect(b_recovery_ip, b_recovery_port, cp->connection->thread->base);

        //第14步 发送数据给recovery
        send_to_recovery(b_primary_ip, b_primary_port, r_cp);
        return 1;
    }
    // 第12****步 backup收到数据请求信息
    else if (ntokens == 3 && strcmp(comm, "DATA_REQUEST") == 0) {
        printf("->->->->-> : receive \"DATA_REQUEST\" from %s : %d\n", inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));

        // 第13****步 向recovery发送数据(这里的ip,port到后期需要修改,这个ip和port是primary信息)
        char ip[16] = "127.0.0.1";
        int port = 11111;
        printf("+++++++++> send data to recovery\n");
        char ipPort[32] = "";
        sprintf(ipPort, "%s.%d", ip, port);
        Segment *seg = loadToMem(ipPort); /* load data from disk to memory */
        char str[1024*10] = "";
        char temp[1024*10] = "";
        segmentToString(seg, temp);
        strcpy(str, "DATA_REPLAY \r\n");
        strcat(str, temp);
        out_string(c, str);
        printf("send data :\n \"%s\"\n", str);


        //send_to_recovery("127.0.0.1", 11111, cp);这里使用这个函数无效
        return 1;
    }
	return -1;
}

int ramcube_server_proxy_new(conn_t *c)
{
	assert(ramcube_config_file);
	
	ConnProxy *cp = (ConnProxy *)calloc(1, sizeof(ConnProxy));
	assert(cp != NULL);

	cp->connection = c;
	cp->m_type = PROXY_TYPE_DATA_SERVER; // this may be modified by TYPE command
	c->ramcube_proxy = (void *)cp;

	//bufferevent *bev = bufferevent_socket_new(pBase, -1, BEV_OPT_CLOSE_ON_FREE);
	

	return 0;
}

int process_command_TYPE(ramcube_token_t *tokens, ConnProxy *cp) 
{
    //int rtn, i;
    int i;
	assert(cp->m_type == PROXY_TYPE_DATA_SERVER);

	char *subcomm = tokens[SUBCOMMAND_TOKEN].value;
	assert(subcomm);
	char *peer_name = tokens[PEER_NAME].value;
	assert(peer_name);
	
	if (strcmp(subcomm, "DATA") == 0) {
		cp->m_type = PROXY_TYPE_DATA_SERVER;
		return 0;
	}
    // backup第10****步，收到recovery请求连接信息
	else if (strcmp(subcomm, "RECOVERY") == 0) {	
		//i am the recovery server for a failed primary, meaning a flow (backup -> me)
		cp->m_type = PROXY_TYPE_RECOVERY_SERVER;
		

		if (set_sin_by_name(peer_name, &cp->m_peerSin) == -1){	
            fprintf(stderr, "ERROR in set_sin_by_name(): failed to init %s.\n", peer_name);
			return -1;
		}

        fprintf(stderr, "process_command_TYPE(): %s (%s:%d) connected\n", subcomm,
				inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
        fprintf(stderr, "process_command_TYPE(): (%s:%d) added to vpRecoveryIn\n",
				inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
		push_back_proxy(vpRecoveryIn, cp);

		return 0;
	}
    else if (strcmp(subcomm, "BACKUP") == 0 || strcmp(subcomm, "PING") == 0 || strcmp(subcomm, "COORDINATOR") == 0) {
		SOCKADDR_IN **pvpSin = NULL;
		
		if (strcmp(subcomm, "BACKUP") == 0) {
			// i am the backup server for my peer, its data being backuped to me
			cp->m_type = PROXY_TYPE_BACKUP_SERVER;
			pvpSin = vpBackupSins;
//			cp->m_pBackupChain = NULL;
			
			//backup\BB\FA\D6Ʊ\E4\C1ˣ\AC\B2\BB\D3\C3CacheChainʵ\CF֣\AC\CF\C2\C3\E6\C1\BD\D0в\BB\D4\D9\D0\E8Ҫ
			//m_pBackupChain = new CacheChain();
			//m_pBackupChain->init_chain(INIT_HASH_SIZE/vpBackupSins.size());

			//printf("init chain for backup server, size = %d\n", vpBackupSins.size());
		} 
        else if (strcmp(subcomm, "PING") == 0){
			// i am the ping server for my peer, its ping msg being sent to me
			cp->m_type = PROXY_TYPE_PING_SERVER;
			pvpSin = vpPingSins;
		}
        else {
            cp->m_type = PROXY_TYPE_COORDINATOR_SERVER;
            pvpSin = vpCoordinatorSins;
        }

		//record peer info (mainly port #) for debugging on one machine
        //rtn =
        set_sin_by_name(peer_name, &cp->m_peerSin);
        //assert(rtn == 0);

		char peerAddr[100];
		strcpy(peerAddr, inet_ntoa(cp->m_peerSin.sin_addr));
		int peerPort = ntohs(cp->m_peerSin.sin_port);

		//the following loop is to ensure the received name is a correct one, i.e., already being stored in pvpSin
		bool isMyPeer = false;
		for (i = 0; i < MAX_NEIGHBORS; i++){
			//printf("comparing peerSin (%s:%d), (%s:%d) for type %d\n", inet_ntoa(m_peerSin.sin_addr), ntohs(m_peerSin.sin_port), 
			//		inet_ntoa((*iter)->sin_addr), ntohs((*iter)->sin_port), m_type);
			if (pvpSin[i] == NULL)
				break;
			if (strcmp(peerAddr, inet_ntoa(pvpSin[i]->sin_addr)) == 0 
				&& peerPort == ntohs(pvpSin[i]->sin_port)) { 
				//check whether it is a backup/ping peer
				isMyPeer = true;
				break;
			}
		}

		if (isMyPeer == false){
			printf("ERROR in looking for name (%s) in my pvpSin (type: %d)\n", 
					peer_name, cp->m_type);
			return -1;
		} 
		else {
			printf("process_command_TYPE(): %s (%s:%d) connected\n", 
					subcomm, peerAddr, peerPort);
			if (cp->m_type == PROXY_TYPE_PING_SERVER) {
				//this ping conn is constructed passively, adding it to vpPingIn
				push_back_proxy(vpPingIn, cp);
				printf("process_command_TYPE(): (%s:%d) added to vpPingIn\n", 
						peerAddr, peerPort);
			} else if (cp->m_type == PROXY_TYPE_BACKUP_SERVER){
				push_back_proxy(vpBackupIn, cp);
				printf("process_command_TYPE(): (%s:%d) added to vpBackupIn\n", 
						peerAddr, peerPort);
            } else if (cp->m_type == PROXY_TYPE_COORDINATOR_SERVER){
                push_back_proxy(vpCoordinatorIn, cp);
                printf("process_command_TYPE(): (%s:%d) added to vpCoordinatorIn\n",
                        peerAddr, peerPort);
            }
		}

		//check whether I need to connect to this backup
		//here we simply assume if a node is my backup/ping, then i must be its
		//backup/ping, too
		int iCompareResult = inet_addr_compare(config.myAddr, config.myPort, 
				peerAddr, peerPort);

		if (iCompareResult < 0) {  
            //inet_aton(ip, &Sin.sin_addr);
            //Sin.sin_port = htons(port);
			enum proxy_type pt = (cp->m_type == PROXY_TYPE_BACKUP_SERVER 
                    ? PROXY_TYPE_BACKUP_CLIENT :
                    cp->m_type == PROXY_TYPE_PING_SERVER ? PROXY_TYPE_PING_CLIENT
                      : PROXY_TYPE_COORDINATOR_CLIENT);
			/*ConnProxy *cp2 =*/ connect_and_return_ConnProxy(cp->connection->thread->base, 
					&cp->m_peerSin, pt);
			//assert(cp2);
		}

		return 0;
	}

	return -1;
}

//第16步 recovery收到信息后向backup 发送信息
ulong ramcube_post_set_data(conn_t *c) 
{
	//int i;
	assert(ramcube_config_file);

	item *it = c->item;
	assert(it);

	ConnProxy *cp = (ConnProxy *)(c->ramcube_proxy);
	assert(cp);
    printf("^^^^^^^^^^^^^^^^^^^^^\n");
    if (cp->m_type == PROXY_TYPE_DATA_SERVER
            || cp->m_type == PROXY_TYPE_RECOVERY_SERVER) {

        // 这里提供了一种通过hash来查询将某个Key-Value对存储到哪个backup上
		ConnProxy *cpBackup = get_backup_proxy_by_key(ITEM_key(it), it->nkey);
		assert(cpBackup);	
        if (cpBackup != NULL) {
            send_unit_to_backup(cpBackup, it, (void *)c);
        }
        printf("*******************\n");
		//out_string(c, "STORED");
		//drive_machine(c);

		return 0;		
	}
	else if (cp->m_type == PROXY_TYPE_BACKUP_SERVER) {
		assert(cp->data_conn_ptr);
		/* TODO: a seperate backup service thread is needed to write backup data to disk*/		
		return (ulong)cp->data_conn_ptr;
		
		char str[100];
		snprintf(str, 100, "BACKUP_REPLY STORED 0 0 0 %lu", (ulong)cp->data_conn_ptr);
		ConnProxy *out_cp = NULL;
		
		/*for (i = 0; i < MAX_NEIGHBORS; i++) {
			if (evutil_sockaddr_cmp((struct sockaddr *)(&vpBackupOut[i]->m_peerSin), 
					(struct sockaddr *)(&cp->m_peerSin), 1) == 0) {		
				out_cp = vpBackupOut[i];
				break;
			}
		}
		*/
		assert(out_cp);
		send_string_via_out_cp(out_cp, str);
		cp->data_conn_ptr = NULL;

		return 1;
	}
    //else if (cp->m_type == PROXY_TYPE_RECOVERY_CLIENT) { /* add by Aaron */
        //send_to_recovery();
        //return 0;

    //}

	return -1;
}

ConnProxy *get_backup_proxy_by_key(const char *key, size_t nkey)
{
	int i;
	for (i = 0; i < MAX_NEIGHBORS; i++) {
		if (vpBackupOut[i] == NULL)
			break;
	}

	if (i == 0) {
		return NULL;
	}

	unsigned int hv = hash(key, nkey, 0);
	int index = hv % i;
	return vpBackupOut[index];
}

//from primary node. pri_sfd: conn fd of the primary receiving data (not the backup conn!)
void send_unit_to_backup(ConnProxy *cpBackupOut, item *unit, void *data_conn_ptr) 
{
    Object obj = setCommandServer(ITEM_key(unit), ITEM_data(unit), unit->nbytes - 2);
    printf("here happend aborted ???\n");
    size_t nBuf;// = evbuffer_get_length(cpBackupOut->m_outputBuffer);
    fprintf(stderr,"++++++++++%lu\n", nBuf);
	//assert(comm);
	//send_string(comm);
	//evbuffer_add_printf(cp->m_outputBuffer, "%s %d\r\n", comm, clifd);
    //evbuffer_drain(cpBackupOut->m_outputBuffer, 2048);
    evbuffer_add_printf(cpBackupOut->m_outputBuffer, "BACKUP_REQUEST %lu\r\n",
            (ulong)data_conn_ptr);

    evbuffer_add(cpBackupOut->m_outputBuffer, obj.command, strlen(obj.command));
    evbuffer_add_printf(cpBackupOut->m_outputBuffer, "%s:%d\r\n", config.myAddr, config.myPort);

    nBuf = evbuffer_get_length(cpBackupOut->m_outputBuffer);
    fprintf(stderr,"++++++++++%lu\n", nBuf);

    //evbuffer_add_printf(cpBackupOut->m_outputBuffer, "set %s 0 0 %d\r\n", 
	//		ITEM_key(unit), unit->nbytes - 2);
	//evbuffer_add_reference(cpBackupOut->m_outputBuffer, ITEM_data(unit), unit->nbytes, 
	//		NULL, NULL);
	
}
//! I am BACKUP_CLIENT, so I will reply the BACKUP_SERVER requests
int process_command_BACKUP_REPLY(ramcube_token_t *tokens, ConnProxy *cp)
{
	assert(cp->m_type == PROXY_TYPE_BACKUP_CLIENT);
	conn_t *cli_conn = (conn_t *)atol(tokens[DATA_CONN_PTR].value);
	
	/*int i;
	for(i = 0; i < MAX_NEIGHBORS; i++) {
		if (gWaitConn[i] == NULL)
			continue;
		if (gWaitConn[i]->sfd == fd) {
			cli_conn = gWaitConn[i];
			gWaitConn[i] = NULL;
			break;
		}		
	}
	*/


	assert(cli_conn);
	out_string(cli_conn, "STORED");
	drive_machine(cli_conn);
	
	return 0;
}

void send_string_via_out_cp(ConnProxy *cp, const char *str)
{
	assert(cp && cp->m_outputBuffer);	
	evbuffer_add_printf(cp->m_outputBuffer, "%s\r\n", str);

	//config.writeBytes += (n + 2);
	//return(evbuffer_write(output, fd));	//bufferevent does this for us?
}

//copied from memcached.c
static size_t ramcube_tokenize_command(char *command, ramcube_token_t *tokens, const size_t max_tokens)
{
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}


//++++++++++++++++++++++++++++++++++ recoveryMasterConnect ++++++++++++++++++++++++++++++++++++++//

//++++++++++++++++++++++++++++++++++ recoveryBackupConnect ++++++++++++++++++++++++++++++++++++++//

/*
 * 第13步 连接recovery
 */
ConnProxy *recoveryBackupConnect(char *ip, int port, struct event_base *pBase) {
    /* check recoverymaster whether connect with me already */
    /*int i;
    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (neighbors[i].in_use == true ) {
            if (strcmp(inet_ntoa(neighbors[i].sin.sin_addr), ip) == 0
                && ntohs(neighbors[i].sin.sin_port) == port) {
                return; // already connected so return
            }
        } else {
            break;
        }
    }*///disconsider this case, if backup is sending or receiving the msg, it will cause error

    printf("---------> connecting recovery... ...\n");
    /* to connect recoverymaster */
    SOCKADDR_IN Sin = {0};
    inet_aton(ip, &Sin.sin_addr);
    Sin.sin_port = htons(port);
    Sin.sin_family = AF_INET;
    bzero(&(Sin.sin_zero), 8);
    ConnProxy *cp = connect_and_return_ConnProxy(pBase, &Sin, PROXY_TYPE_RECOVERY_CLIENT);
    return cp;

}

// recovery第11***步 向backup发送数据请求
void send_request_to_backup(ConnProxy *cp) {
    printf("+++++++++> send request data to backup\n");
    evbuffer_add_printf(cp->m_outputBuffer, "DATA_REQUEST %lu\r\n",
            (ulong)cp);
}



// 第14步 发送数据给recovery
void send_to_recovery(char *ip, int port, ConnProxy *cp)
{
    printf("+++++++++> send data to recovery: ");
    char ipPort[32] = "";
    sprintf(ipPort, "%s.%d", ip, port);


    clock_t start, finish; //生命start和finish是两个时间
    double time; //定义运行时间

    start = clock(); //获取开始时间
    Segment *seg = loadToMem(ipPort);
    finish = clock(); //获取完成时间
    time = (double)(finish - start) / CLOCKS_PER_SEC; //CLOCKS_PER_SEC，它用来表示一秒钟会有多少个时钟计时单元，进行计算，完成的时间减去开始的时间获得算法运行时间
    printf( "运行时间为\n%f 秒\n",time);//显示

    start = clock(); //获取开始时间
    char *str = (char*)malloc(sizeof(char) * 1024 * 1024 * 1024 * 4);
    segmentToString(seg, str);
    finish = clock(); //获取完成时间
    time = (double)(finish - start) / CLOCKS_PER_SEC; //CLOCKS_PER_SEC，它用来表示一秒钟会有多少个时钟计时单元，进行计算，完成的时间减去开始的时间获得算法运行时间
    printf( "运行时间为\n%f 秒\n",time);//显示
    fprintf(stderr, "准备发送数据...");

 /*
    //测试
    char *str = (char*)malloc(sizeof(char) * 1024 * 1024 * 1024);//test

    memset(str, 'a', 1024 * 1024 * 1024);//test

    long long start=getSystemTime();//test
    printf("start time: %lld ms\n", start);//test
*/
    evbuffer_add_printf(cp->m_outputBuffer, "RECOVERY_REQUEST %lu\r\n",
            (ulong)cp);
    //printf("\"%s\"", str);
    evbuffer_add(cp->m_outputBuffer, str, strlen(str));
    evbuffer_add_printf(cp->m_outputBuffer, "%s:%d\r\n", config.myAddr, config.myPort);

}

/*
 * convert segment formatting to string
 */
void segmentToString(Segment *seg, char *str) {
    Seglet *let = seg->segleter;
    while (let != NULL) {
        strcat(str, let->objector->command);
        let = let->next;
    }
}

/*+++++++++++++++++++++++++++++++++++++ 心跳机制 ++++++++++++++++++++++++++++++++++*/


/*
 * primary第三步
 * HB_cb是heartbeat回调函数
 */
static void HB_cb(int fd, short event, void *arg) {
    ConnProxy *cp = (ConnProxy *)arg;
    if (sendHeartBeat(cp) == 1) {
        struct timeval tv;
        tv.tv_sec = 1; //时间间隔
        tv.tv_usec = 0; //微秒数
        // 事件执行后,默认就被删除,需要重新add,使之重复执行
        event_add(&(cp->evt), &tv);
    }
    /*else {
        Recovery(cp);
    }*/
}

/*
 * primary第二步
 */
void HeartBeat(ConnProxy *cp, struct event_base *pBase) {
    struct timeval tv; //设置定时器
    //struct event ev; //事件
    tv.tv_sec = 1; //时间间隔
    tv.tv_usec = 0; //微秒数
    static bool initialized = false;
    if(initialized) {
        evtimer_del(&(cp->evt));
    } else {
        initialized = true;
    }
    evtimer_set(&(cp->evt), HB_cb, cp);//初始化事件，并设置回调函数
    event_base_set(pBase, &(cp->evt));
    event_add(&(cp->evt), &tv); //注册事件
}

/*
 * primary第四步
 * 向recovery发送“HEARTBEAT”
 */
int sendHeartBeat(ConnProxy * cp) {
    char peerIp[16] = "";
    int peerPort;
    strcpy(peerIp, inet_ntoa(cp->m_peerSin.sin_addr)); //是哪一个启动链接的，我就向那个发送Heartbeat
    peerPort = ntohs(cp->m_peerSin.sin_port);
    /*if (cp->outTimes < MAX_HEARTBEAT_TIMES) {
        int i;
        // check whether to send "crash" instead of "heartbeart"
        for (i = 0; i < MAX_MSG_BUFFER; ++i) {
            if (bokenMasterTable[i].used == true
                    && strcmp(bokenMasterTable[i].b_backup_ip, peerIp) == 0
                    && bokenMasterTable[i].b_backup_port == peerPort) {
                char str[64] = "";
                bokenMasterTable[i].used = false;
                sprintf(str, "CRASH\n\r%s:%d RECOVERY %s:%d", bokenMasterTable[i].b_master_ip, bokenMasterTable[i].b_master_port,
                        bokenMasterTable[i].b_recovery_ip, bokenMasterTable[i].b_recovery_port);
                strcpy(cp->sendInfo, str);
                break;
            }
        }
        if (i == MAX_MSG_BUFFER) { //no node crash*/
        strcpy(cp->sendInfo, "HEARTBEAT");
        //}


        evbuffer_add_printf(cp->m_outputBuffer, "%s\r\n", cp->sendInfo);
        //evbuffer_add(cp->m_outputBuffer, cp->sendInfo, strlen(cp->sendInfo));
        ++(cp->outTimes);
        printf("\n*>*>*>*>*> : send \"%s\"  to  %s : %d\n", cp->sendInfo, peerIp, peerPort);
        return 1;
    /*} else {
        sprintf(cp->sendInfo, "CRASH:%s:%d", inet_ntoa(cp->m_peerSin.sin_addr),
                ntohs(cp->m_peerSin.sin_port));  //int to char
        return 0;
    }*/
}

void Recovery(ConnProxy * cp) {
    //TODO should read from a conf
    char b_master_ip[16];
    strcpy(b_master_ip, inet_ntoa(cp->m_peerSin.sin_addr));
    int b_master_port = ntohs(cp->m_peerSin.sin_port);
    char b_backup_ip[16] = "127.0.0.1";
    int b_backup_port = 11114;
    char b_recovery_ip[16] = "127.0.0.1";
    int b_recovery_port = 11121;
    int i;

    for (i = 0; i < MAX_MSG_BUFFER; ++i) {
        if (bokenMasterTable[i].used == false) {
            bokenMasterTable[i].used = true;
            strcpy(bokenMasterTable[i].b_backup_ip, b_master_ip);
            strcpy(bokenMasterTable[i].b_master_ip, b_backup_ip);
            strcpy(bokenMasterTable[i].b_recovery_ip, b_recovery_ip);
            bokenMasterTable[i].b_master_port = b_master_port;
            bokenMasterTable[i].b_backup_port = b_backup_port;
            bokenMasterTable[i].b_recovery_port = b_recovery_port;
            return;
        }
    }

    /*
    int i;
    for (i = 0; i < MAX_NEIGHBORS; ++i) {
        if (vpPingIn[i] == NULL) {
            break;
        } else {
            char *pingIp = inet_ntoa(vpPingIn[i]->m_peerSin.sin_addr);
            int pingPort = ntohs(vpPingIn[i]->m_peerSin.sin_port);
            if (strcmp(ip, pingIp) == 0 && port == pingPort){
            printf("*>*>*>*>*> : send \"%s\" to %s : %d\n", cp->sendInfo, pingIp, pingPort);
            evbuffer_add_printf(vpPingIn[i]->m_outputBuffer, "%s\r\n", cp->sendInfo);
            return;
            }
        }
    }
    for (i = 0; i < MAX_NEIGHBORS; ++i) {
        if (vpPingOut[i] == NULL) {
            break;
        } else {
            char *pingIp = inet_ntoa(vpPingOut[i]->m_peerSin.sin_addr);
            int pingPort = ntohs(vpPingOut[i]->m_peerSin.sin_port);
            if (strcmp(ip, pingIp) == 0
                   && port == pingPort){
            printf("*>*>*>*>*> : send \"%s\" to %s : %d\n", cp->sendInfo, pingIp, pingPort);
            evbuffer_add_printf(vpPingOut[i]->m_outputBuffer, "%s\r\n", cp->sendInfo);
            return;
            }
        }
    }*/


}





/*
 * recovery第七步
 */

static void recovery_cb(int fd, short event, void *arg) {
    ConnProxy *cp = (ConnProxy *)arg;
    int i;
    // 在PrimaryTable中找到对应的primary信息
    for (i = 0; i < MAX_PRIMARY; ++i) {
        if (strcmp(PrimaryTable[i].b_primary_ip, inet_ntoa(cp->m_peerSin.sin_addr)) == 0
                   && PrimaryTable[i].b_primary_port == ntohs(cp->m_peerSin.sin_port)) {
            ++PrimaryTable[i].MaxTimes; //次数加1
            printf("MaxTimes: %d\n", PrimaryTable[i].MaxTimes);

            // 如果有2s没有收到primary心跳，则向coordinator发送信息
            // recovery第八步，向coordiantor发送请求信号
            // 这里将定backup的IP：PORT为：127.0.0.1：:11114
            if (PrimaryTable[i].MaxTimes > 2) {
                //!ConnProxy *b_cp = recoveryBackupConnect("127.0.0.1", 11114, cp->connection->thread->base);
                //!recovery第11**步 向backup发送数据请求
                //!send_request_to_backup(b_cp);

                // 第八步 开始发送信息
                ConnProxy *c_cp2 = vpCoordinatorOut[0];
                evbuffer_add_printf(c_cp2->m_outputBuffer, "CRASH\r\n%s:%d\r\n", inet_ntoa(cp->m_peerSin.sin_addr), ntohs(cp->m_peerSin.sin_port));
                printf("send msg to coordinator!\n");
                return;
            }
        }
    }

    struct timeval tv;
    tv.tv_sec = 1; //时间间隔
    tv.tv_usec = 0; //微秒数
    // 事件执行后,默认就被删除,需要重新add,使之重复执行
    event_add(&(cp->evt), &tv);
}

/*
 * recovery第六步
 */
void autoAddTimes(ConnProxy *cp, struct event_base *pBase) {
    struct timeval tv; //设置定时器
    //struct event ev; //事件
    tv.tv_sec = 1; //时间间隔
    tv.tv_usec = 0; //微秒数
    static bool initialized = false;
    if(initialized) {
        evtimer_del(&(cp->evt));
    } else {
        initialized = true;
    }
    evtimer_set(&(cp->evt), recovery_cb, cp);//初始化事件，并设置回调函数
    event_base_set(pBase, &(cp->evt));
    event_add(&(cp->evt), &tv); //注册事件
}

















