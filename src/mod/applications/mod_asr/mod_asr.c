#include <string.h>
#include <switch.h>
struct switch_asr_info {

	char *taskid;
	switch_core_session_t *session;
	switch_media_bug_t *bug;
	switch_channel_t *channel;
};
//ASR callback
static switch_bool_t asr_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	struct switch_asr_info *asr_info = (struct switch_asr_info *)user_data;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ASR channel:%s\n",
					  switch_channel_get_name(asr_info->channel));
	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ASR Start Init:%s\n",
						  switch_channel_get_name(asr_info->channel));
		break;

	case SWITCH_ABC_TYPE_READ_REPLACE:
		// switch_frame_t *frame = switch_core_media_bug_get_read_replace_frame(bug);
		//
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ASR Start Close:%s\n",
						  switch_channel_get_name(asr_info->channel));
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}
// 这是您为卸载或重新加载模块进行任何清理的地方，您应该释放状态处理程序、事件保留等。您还应该与关闭运行时线程同步（通常使用类似于关闭函数的共享“运行”变量设置为运行时函数注意到的某个值，设置为第三个值，然后退出）。
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown);
// 这是模块的运行时循环，从这里你可以监听套接字，产生新的线程来处理请求。等等
// SWITCH_MODULE_RUNTIME_FUNCTION(mod_asr_runtime);
// 使用这个宏定义了模块的加载函数，在这个函数中你应该初始化任何全局结构，连接任何事件或状态处理程序等。如果你返回除了
// SWITCH_STATUS_SUCCESS 以外的任何东西，模块将不会继续被加载。
SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load);
// 模块定义(模块name，load函数，shutdown函数，runtime函数)
SWITCH_MODULE_DEFINITION(mod_asr, mod_asr_load, mod_asr_shutdown, NULL);
void trim(char *string)
{
	char *p;
	if ((p = strchr(string, '\n'))) { *p = '\0'; }
	if ((p = strchr(string, '\r'))) { *p = '\0'; }
}
SWITCH_STANDARD_API(task_api_function)
{
	// SWITCH_STANDARD_API have three args:(cmd, session, stream)
	char *data;
	int argc;
	char *argv[3];
	char *action;
	char *taskid;
	switch_core_session_t *usession;
	switch_channel_t *pchannel;
	// switch_media_bug_t *bug;
	switch_status_t status;
	struct switch_asr_info *asr_info;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "cmd: %s \n", cmd);
	data = strdup(cmd);
	trim(data);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "data: %s \n", data);
	if (!(argc = switch_separate_string(data, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid usage\n");
		goto done;
	}
	action = argv[0];
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "cmd: %s \n", action);
	taskid = argv[1];
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "taskid: %s \n", taskid);
	usession = switch_core_session_locate(taskid);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "session: %s \n", switch_core_session_get_name(usession));
	pchannel = switch_core_session_get_channel(usession);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "channel: %s \n", switch_channel_get_name(pchannel));

	if (!strcasecmp(action, "start")) {
		stream->write_function(stream, "start OK\n");
		if (!(asr_info =
				  (struct switch_asr_info *)switch_core_session_alloc(usession, sizeof(struct switch_asr_info)))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "alloc error\n");
			goto done;
		}
		// asr_info = (struct switch_asr_info *)malloc(sizeof(struct switch_asr_info));
		// memset(asr_info, 0, sizeof(*asr_info));
		asr_info->taskid = taskid;
		asr_info->session = usession;
		asr_info->channel = pchannel;

		if ((status = switch_core_media_bug_add(usession, "asr", NULL, asr_callback, asr_info, 0,
												SMBF_READ_REPLACE | SMBF_NO_PAUSE | SMBF_ONE_ONLY, &(asr_info->bug))) !=
			SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "get media bug error\n");
			goto done;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "status: %d \n", status);
		switch_channel_set_private(pchannel, "asr", asr_info);

		// switch_channel_set_flag(pchannel, CF_HOLD);

		switch_core_session_rwunlock(usession);
	} else if (!strcasecmp(action, "stop")) {
		stream->write_function(stream, " stop OK\n");

		// switch_channel_set_private(pchannel, "asr", NULL);
		// switch_core_media_bug_remove(usession, &bug);

		// switch_channel_clear_flag(pchannel, CF_HOLD);
		switch_core_session_rwunlock(usession);
	}
done:
	switch_safe_free(data);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "task api exec\n");
	return SWITCH_STATUS_SUCCESS;
}
// 实现加载模块时被调函数的逻辑
SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load)
{
	switch_api_interface_t *api_interface;
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_asr load\n");
	// register API
	SWITCH_ADD_API(api_interface, "task", "task api", task_api_function, "");

	return SWITCH_STATUS_SUCCESS;
}
// 实现模块运行时被调函数的逻辑
// SWITCH_MODULE_RUNTIME_FUNCTION(runtime)
// {
// 	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_asr runtime\n");
// 	return SWITCH_STATUS_SUCCESS;
// }
// 实现模块停止时被调函数的逻辑
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_asr showdown\n");
	return SWITCH_STATUS_SUCCESS;
}
