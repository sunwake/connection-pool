#include "log.h"
#include <stdio.h>
#include <unistd.h>// usleep
static conn_pool* conn_pool::m_instance=NULL;//init
conn_pool::conn_pool(){
	// becaue of singel_instance  design pattern  ,we coule just init some config less variables.
	is_interrupt = false;
	pthread_mutex_init(&m_mutex,NULL);
	m_is_destruct = false;
	//m_instance=NULL;
	m_useN =0;
	m_idleN =0;
	m_realN = 0;
}
static conn_pool* conn_pool::get_instance(){
	if(NULL == m_instance ){
		m_instance = new conn_pool();
	}
	return m_instance;
}
// atomic add ,sub
inline unsigned int conn_pool::add_useN(unsigned int n)
{
	return __sync_add_and_fetch (&m_useN, n);
}
inline unsigned int conn_pool::sub_useN(unsigned int n)
{
	return __sync_sub_and_fetch (&m_useN, n);
}
inline unsigned int conn_pool::add_idleN(unsigned int n)
{
	return __sync_add_and_fetch (&m_idleN, n);
}
inline unsigned int conn_pool::sub_idleN(unsigned int n)
{
	return __sync_sub_and_fetch (&m_idleN, n);
}
inline unsigned int conn_pool::add_realN(unsigned int n)
{
	return __sync_add_and_fetch (&m_realN, n);
}
inline unsigned int conn_pool::sub_realN(unsigned int n)
{
	return __sync_sub_and_fetch (&m_realN, n);
}	

long conn_pool::get_time_insecond(){
	return time((time_t*)NULL);
}

msg_mail* conn_pool::create_conn(){
	bool is_logined = false;
	// here should give paramers
	msg_mail *ret = new msg_mail(const std::string& usrname, const std::string& passwd,
		const std::string& servip,const unsigned short& port = 5677);
	is_logined = ret->login();
	if(is_logined){
		return ret;
	}
	// login failed
	is_logined = ret->relogin();
	if(is_logined){
		return ret;
	}
	
	ret->close();
	ret =NULL;
	return ret;
}
/*
CONF_POOL_TIMEOUT_STR		CONF_DEF_POOL_TIMEOUT		m_timeout;
CONF_POOL_INIT_NUM_STR		CONF_DEF_POOL_INIT_NUM		m_initN;
CONF_POOL_MAX_NUM_STR		CONF_DEF_POOL_MAX_NUM		m_minIdleN;
CONF_POOL_MIN_IDLE_NUM_STR	CONF_DEF_POOL_MIN_IDLE_NUM	m_maxIdleN;	
CONF_POOL_MAX_IDLE_NUM_STR	CONF_DEF_POOL_MAX_IDLE_NUM	m_maxN;	
CONF_POOL_MAX_IDLE_TIME_STR	CONF_DEF_POOL_MAX_IDLE_TIME	m_max_idle_time;

*/
bool conn_pool::init_conn_pool(Config& myconf){
	
	// int  m_initN connections
	int fail_count = 0;
	int max_fail_count = m_initN;
	msg_mail* p_mail;
	long now;
	for(int i =0;;i<m_initN){
		p_mail = create_conn();
		if (NULL ! = p_mail ){
			now =get_time_insecond();
			m_map_idle_conns.insert(pair(now,p_mail));
			add_idleN(1);// m_idleN++;
			add_realN(1);//m_realN++;
			i++;
			
			continue;// continue to create another connection.
		}
		//create one connection failed
		fail_count++;
		LOG_DEBUG("in init_conn_pool:create_conn failed ,time %d\n",fail_count);
		// ´ýÕå×Ã
		if(fail_count >= max_fail_count){
			destroy_conn_pool();// don`t check 
			LOG_ERROR("in init_conn_pool:create_conn failed \n");
			return false;
		}
	}
	LOG_ERROR("in init_conn_pool:create_conn success!!! ,but failed %d times\n",fail_count);
	
	return true;
}
bool conn_pool::destroy_conn_pool(){
	
	unsigned int idleN =0;
	int wait_count = 100;
	// kill keep_balance thread
	m_is_destruct = true;
	wake_check_thread();
	pthread_join(m_keep_balance_threadId);
	
	msg_mail* p_mail = NULL;
	pthread_mutex_lock(m_mutex);// lock
	
	map_it_t it = m_map_idle_conns.begin();
	for(;it != m_map_idle_conns.end();it++){
		delete (*it).second;
	}
	
	
	idleN=sub_idleN(0);
	sub_idleN(idleN);// set idleN = 0;
	sub_realN(idleN);
	m_map_idle_conns.clear();
	pthread_mutex_unlock(m_mutex);// unlock
	//wait for using connections release
	// the longest wait time is 10s
	while(add_useN(0)&&wait_count){ 
		usleep(10*10*10*10*10);
		wait_count--;
	}
	
	pthread_mutex_lock(m_mutex);// lock
	it = m_map_idle_conns.begin();
	for(;it != m_map_idle_conns.end();it++){
		delete (*it).second;
	}
	m_map_idle_conns.clear();
	pthread_mutex_unlock(m_mutex);// lock
	
	pthread_mutex_destroy(&m_mutex);
	m_instance =NULL;
}
// return NULL if out of connection  or connction disconnect 
msg_mail* conn_pool::get_conn(){
	
	//m_is_destruct
	if(m_is_destruct){
		LOG_DEBUG("connection pool is destruct!\n");
		return NULL;
	}
	// avoid goto
	msg_mail* p_mail = NULL;
	long start_time =0;
	long now;
	unsigned int idle_num = add_idleN(0); 
	if(idle_num > 0){
		pthread_mutex_lock(m_mutex);// lock
		
		map_rit_t rit = m_map_idle_conns.rbegin();// get the lastest elem
		assert(m_map_idle_conns.rend() == it );// make sure that it is not the end 
		p_mail = (*it).second;
		start_time = (*it).first;
		m_map_idle_conns.erase( it ); // pop head 
				
		pthread_mutex_unlock(m_mutex);// lock
		
		add_useN(1);
		sub_idleN(1);
		
		// if conn_pool is pool,wake up check thread
		if(idle_num < m_minIdleN){
			wake_check_thread();
		}
		
		
		//check connection
		now =get_time_insecond();
		if((now - start_time > m_timeout)|| p_mail->check_conn()){
			if(!p_mail->relogin()){
				
				p_mail->close();
				// update count
				sub_useN(1);
				sub_realN(1);
				LOG_DEBUG("in get_conn:relogin failed!\n");
				p_mail=NULL;//just set NULL;
			}
		} 
		
	
	}else{
		LOG_DEBUG("connection pool is out of connctions!\n");
	}
	
	return 	p_mail;
	
	
}


bool conn_pool::release_conn(msg_mail* conn){
	
	if (NULL == conn){
		LOG_DEBUG("APP returns a NULL conn!\n");
		return true;
	}
	long now;
	now =get_time_insecond();
	pthread_mutex_lock(m_mutex);// lock
	m_map_idle_conns.insert(pair(now,p_mail));
	sub_useN(1);
	add_idleN(1);
	pthread_mutex_unlock(m_mutex);// unlock
	

	
	return true;
		
}
bool conn_pool::get_running_info(pool_running_info_t& info){
	info.total_num = add_realN(0);
	info.using_num = add_useN(0);
	info.idle_num = add(0);
	return true;
}
bool conn_pool::pool_config_info_t(pool_config_info_t& info){
	info.timeout = m_timeout;
	info.init_num = m_initN;
	info.min_idle_num = m_minIdleN;
	info.max_idle_num = m_maxIdleN;
	info.balance_ilde_num = m_balanceIdleN;
	info.max_num = m_maxN;
	info.max_idle_time = m_max_idle_time;
	return true;
}
	// increase n conn  when idleN < m_minIdleN
int conn_pool::increase_conns(unsigned int n,unsigned int max_fail_count){
	msg_mail* p_mail;
	long now;
	int crate_count =0;
	int fail_count =0;
	for(int i =0;i < n;){
		p_mail = create_conn();
		if (NULL ! = p_mail ){
			now =get_time_insecond();
			pthread_mutex_lock(m_mutex);
			m_map_idle_conns.insert(pair(now,p_mail));
			add_idleN(1);// m_idleN++;
			add_realN(1);//m_realN++;
			pthread_mutex_unlock(m_mutex);

			crate_count++;
			i++;
			
			continue;// continue to create another connection.
		}
		//create one connection failed
		fail_count++;
		LOG_DEBUG("in increase_conns:create_conn failed %dth\n",fail_count);
		// ´ýÕå×Ã
		if(fail_count >= max_fail_count){
			LOG_ERROR("in increase_conns:create_conn failed \n");
			//return crate_count;
			break;//break for
		}
	}
	return crate_count;	
}
	// close over number connections and timeout connections
	// although this opposite design rule ,we do this major to do effect
int conn_pool::close_over_and_timeout_conns(unsigned int n,unsigned int end_time){
	map_it_t it_tmp;
	map_conns_t map_tmp;
	int erase_pos=0;
	int timeout_pos = 0;
	int over_pos = n-1;
	assert( over_pos > -1 );
	
	pthread_mutex_lock(m_mutex);
	
	// get timeout position in map;
	for (it_tmp = m_map_idle_conns.begin();\
			(it_tmp->first<(end_time+1))&&(it_tmp!=m_map_idle_conns.end());
			it_tmp++){
		timeout_pos++;
	}
	// get the min position of timeout ,min_idle,over_num in map;
	/*
	if(timeout_pos< (m_minIdleN-1) ){
		erase_pos = (m_minIdleN-1);
		LOG_DEBUG("the connections before minIdle is out of time! it`s position is %d(start from 0)\n",timeout_pos);
	}else{
		erase_pos = timeout_pos < over_pos ? timeout_pos :over_pos;
	}
	*/
	/*
	timeout = 45s
	m_minIdleN =2
	idle_num =53
	minIdle_pos = idle_num-1 -m_minIdleN =52
	|__|__|__|__|__|__|__|......|__|__|__|
	0  1  2  3  4  5  6  7     49 50 51  52
	      |              |         |     |
	   over_pos timeout_pos minIdle_pos 
						 |
					erase_pos
	*/
	unsigned int idle_num =add_idleN(0);
	erase_pos = timeout_pos > over_pos ? timeout_pos :over_pos;
	if(erase_pos > (idle_num-1-m_minIdleN) ){
		LOG_DEBUG("the timeout_pos or the over_pos is before minIdle pos! timeout_pos(%d); over_pos(%d);minIdle_pos(%d)\n",timeout_pos,over_pos,m_minIdleN-1);
		erase_pos = idle_num-1-m_minIdleN;
		
	}
	//too complex
	/*
	int i_once = 0;
	for(it_tmp = m_map_idle_conns.begin(),i_once=0;i_once<=(erase_pos+1);i_once++,it_tmp++){}
	*/
	// move it_tmp to erase_pos+1
	it_tmp = m_map_idle_conns.begin();
	for(int i =0;i<erase_pos+2;i++){
		it_tmp++;
	}
		
	// get the quality part of m_map_idle_conns;//they are in behind
	map_tmp.insert(it_tmp,m_map_idle_conns.end());
	// now m_map_idle_conns contains only quality connections
	m_map_idle_conns.swap(map_tmp);
	sub_idleN(erase_pos+1);
	sub_realN(erase_pos+1);
	pthread_mutex_unlock(m_mutex);// now we can use the map 
	
	// close the connections from 0 to erase_pos in map
	it_tmp = map_tmp.begin();
	for(int i=0;i<(erase_pos+1);i++){
		//close_conn(msg_mail* conn);
		
		close_conn((*it_tmp)->second);
		it_tmp++;
	}
	map_tmp.clear();
	
	return true;
}
int conn_pool::auto_adjust_conns(){
	unsigned int num = 0;
	unsigned int idle_num =add_idleN(0);
	int ret;
	
	if(idle_num <m_minIdleN){// idleN <min_idleN:need to increase
		num =  m_balanceIdleN -idle_num;
		//increase_conns(unsigned int n,unsigned int max_fail_count)
		ret = increase_conns(num,num);
		if( ret != num){LOG_ERROR("auto_adjust_conns: we want to increase %d connections,but in fact we only create %d!!!\n",num,ret);}
	}else
	{
		bool flag = false;
		long dead_time;
		//check if need to close connections or not 
		do{
			if(idle_num > m_maxIdleN){flag = true; break;/*break do-while(0)*/}
			
			dead_time = get_time_insecond() - m_timeout;// deadline time 
			map_it_t it_tmp;
			
			//check if the begin of the map(the minist time) is early than the dead time 
			pthread_mutex_lock(m_mutex);
			it_tmp =  m_map_idle_conns.begin();
			// if the first connection in m_map_idle_conns is early than dead_time
			flag =((*it_tmp).first < dead_time )? true :false;
			pthread_mutex_unlock(m_mutex);
			
		}while(0);
		
		if(flag){
			num = idle_num - m_balanceIdleN;
			//close_over_and_timeout_conns(unsigned int n,unsigned int end_time);
			close_over_and_timeout_conns(num,dead_time);
			LOG_DEBUG("auto_adjust_conns:close %d connections \n",num);
		}
	}

}
//this is a call back function for pthread_create() whick keep balance of 
//the conn pool 
void* conn_pool::conn_pool_keep_balance(void* ){
	int i =0;
	m_keep_balance_threadId =pthread_self();
	while(1){
		//sleep m_maxIdleN seconds or be wake up
		for(i =0;i<m_maxIdleN*10;i++){//check every 0.1s 
			if(true == is_interrupt){// the wakeup flag is  set true by increase_conns()
				is_interrupt = false;//reset the flag
				break;//break for 
			}
			usleep(10*10*10*10*10);// 100000us,0.1s
		}
		if(m_is_destruct){// the  object destruct
			break;// break while:goes to return 
		}
		auto_adjust_conns();//work
	}
	return (void*)0;
}