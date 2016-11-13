/*
*author:sunwake(sunwake at foxmail dot com)
*date:2016-10-31
*desc:
*this is a connection pool for msg_mail connect. 
*it could auto increases or decreases conn number,
*just like a tomcat`s connection pool 
*/
#include <stdlib>// for multimap?
#include <pthread.h>// for lock
#include "msg_mail.h"
#include "config.h"
/*============================================================
			 minIdleN 
			  |----|
|_____|_______|____|___|____|__________|    connection num
0    initN   useN     realN             maxN
			  |-IdleN -|
			  |--maxIdleN---|


initN:标量，初始连接数量，在没有连接的时候，realN可能会比这个数目少
useN：标量，正在使用的连接数量，有可能为0
minIdleN：差量，最小空闲连接数量，任何时候 都至少保持
		minIdleN数量的连接（minIdleN<initN）
maxIdleN:差量，最大空闲连接数量，当大于这个数量时，开始剔除额外的空闲连接（大于UseN+maxIdleN的部分）。
idleN：差量，空闲的连接数量，连接池任何时候都保持一定数量的空闲连接，
		以备不时之需。（minIdleN< idleN<maxIdleN）
realN：标量，实际连接数量（realN = UseN + idleN）
maxN：标量，最大连接数，超过这个数，不再提供连接。
balanceIdleN:= (minIdleN+maxIdleN)/2 (暂且定这个数吧)（？minIdleN+(maxIdleN-minIdleN）*0.8？）

 – maxN直接浮动。在realN中，又有minIdleN – maxIdleN个数的空闲连接随时待命。
数据库连接池中连接的数量是动态增长的，其连接数量（realN）永远在minIdleN
连接池中连接数量动态调整的规则：
①	连接池中空闲连接idleN的数量小于minIdleN时，自动增加至balanceIdleN个连接。（在检测线程和工作线程中调整）
②	连接池中空闲连接idleN的数量大于maxIdleN时，自动减少至balanceIdleN个连接。（在检测线程中调整）
③	连接池中空闲连接idle connection的数量minIdleN<idleN<maxIdleN，并且有部分idle connection超时时（> timeout【timeout是自己设定的】），关闭超时那部分idle connection（在保证idleN >=minIdleN的情况下 ）
④	对于idle connection中<minIdleN的部分并且已经超时的连接，采用“鸵鸟算法”。
⑤	每max_idle_time调整一次（检测线程）。（默认15s，like tomcat）


==================================================================*/
#define CONF_DEF_POOL_TIMEOUT		45//45s		
#define CONF_DEF_POOL_INIT_NUM		3		
#define CONF_DEF_POOL_MAX_NUM		8// the max speed of anti fire_wall is 6 files per second
#define CONF_DEF_POOL_MIN_IDLE_NUM	2
#define CONF_DEF_POOL_MAX_IDLE_NUM	4
#define CONF_DEF_POOL_MAX_IDLE_TIME	15// 15s 


#define CONF_POOL_TIMEOUT_STR		connection_timeout
#define CONF_POOL_INIT_NUM_STR		connection_pool_init_num
#define CONF_POOL_MAX_NUM_STR		connection_pool_max_num
#define CONF_POOL_MIN_IDLE_NUM_STR	connection_pool_min_idle_num
#define CONF_POOL_MAX_IDLE_NUM_STR	connection_pool_max_idle_num
#define CONF_POOL_MAX_IDLE_TIME_STR	connection_pool_max_idle_time

using namespace std;

typedef struct Running_info{
	unsigned int using_num;
	unsigned int idle_num;
	unsigned int total_num;
}pool_running_info_t;
typedef struct Config_info{
	unsigned int timeout;
	unsigned int init_num;
	unsigned int min_idle_num;
	unsigned int max_idle_num;
	unsigned int balance_ilde_num;
	unsigned int max_num;
	unsigned int max_idle_time;
}pool_config_info_t;

class conn_pool{
public:
	static conn_pool* get_instance(Config& myconf);
	bool init_conn_pool();
	msg_mail& get_conn();
	bool release_conn(msg_mail& mail);
	// for count 
	bool get_running_info(pool_running_info_t& info);
	bool get_config_info(pool_config_info_t& info);
	// this func is for a loop thread which keep conn_pool in a health station
	void* conn_pool_keep_balance(void* );
private:
	conn_pool();// no constructer with 0 param
	conn_pool(Config& myconf);
	~conn_pool();
	conn_pool(const conn_pool& conn);// no copy constructer
	conn_pool& operator = (const conn_pool& conn);// no sign constructer
	void wake_check_thread(){is_interrupt = true;}
	inline unsigned int add_useN(unsigned int n);
	inline unsigned int sub_useN(unsigned int n);
	inline unsigned int add_idleN(unsigned int n);
	inline unsigned int sub_idleN(unsigned int n);
	inline unsigned int add_realN(unsigned int n);
	inline unsigned int sub_realN(unsigned int n);
private:
	int auto_adjust_conns();
	// increase n conn  when idleN < m_minIdleN
	int increase_conns(unsigned int n);
	// close over number connections and timeout connections
	// although this opposite design rule ,we do this major to do effect
	int close_over_and_timeout_conns(unsigned int n,unsigned int start_time);
	////close the connections that is behind position of minIdle and out of time
	//int close_conn_timeout(long start_time);
	
	msg_mail* create_conn();
	bool  close_conn(msg_mail* conn);
	long get_time_insecond();
	bool destroy_conn_pool();
	
	volatile bool is_interrupt;// is interrupt by another thread
	pthread_mutex_t m_mutex;//thread locker
	static conn_pool* m_instance;
	volatile bool m_is_destruct;
	pthread_t m_keep_balance_threadId;
	
	volatile unsigned int m_useN;// atomic_t?
	volatile unsigned int m_idleN;
	volatile unsigned int m_realN;
	
	
	unsigned int m_timeout;
	unsigned int m_initN;
	unsigned int m_minIdleN;
	unsigned int m_maxIdleN;
	unsigned int m_balanceIdleN;// balance idle connection number to avoid create or free conn frequently
	unsigned int m_maxN;
	unsigned int m_max_idle_time;
	
	
	multimap<unsigned long,msg_mail*> m_map_idle_conns;
	typedef multimap<unsigned long,msg_mail*> map_conns_t;
	//typedef multimap<unsigned long,msg_mail*>::reverse_iterator rmap_it_t;
	typedef multimap<unsigned long,msg_mail*>::iterator map_it_t;
	//typedef multimap<unsigned long,msg_mail*>::iterator map_rit_t;
	typedef multimap<unsigned long,msg_mail*>::reverse_iterator map_rit_t;

	
}