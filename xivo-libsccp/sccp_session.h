#ifndef SCCP_SESSION_H_
#define SCCP_SESSION_H_

struct sccp_cfg;
struct sccp_device;
struct sccp_device_registry;
struct sccp_msg;
struct sccp_session;
struct sockaddr_in;

/*!
 * \brief Create a new session (astobj2 object).
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_session *sccp_session_create(struct sccp_cfg *cfg, struct sccp_device_registry *registry, struct sockaddr_in *addr, int sockfd);

/*!
 * \brief Run the session.
 *
 * \note This function exit only when the session stops, either "naturally"
 *       or after a call to sccp_session_stop.
 */
void sccp_session_run(struct sccp_session *session);

/*!
 * \brief Stop the session.
 *
 * \retval 0 on sucess
 * \retval non-zero on failure
 */
int sccp_session_stop(struct sccp_session *session);

/*!
 * \brief Reload the session and the associated device.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_session_reload_config(struct sccp_session *session, struct sccp_cfg *cfg);

/*!
 * \brief Function type for device task callback
 *
 * \note Part of the device API.
 */
typedef void (*sccp_device_task_cb)(struct sccp_device *device, void *data);

/*!
 * \brief Add a device task.
 *
 * \note Must be called only from the session thread.
 * \note Part of the device API.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_session_add_device_task(struct sccp_session *session, sccp_device_task_cb callback, void *data, int sec);

/*!
 * \brief Remove a device task.
 *
 * \note Must be called only from the session thread.
 * \note Part of the device API.
 */
void sccp_session_remove_device_task(struct sccp_session *session, sccp_device_task_cb callback, void *data);

/*
 * XXX called to force the session thread to call sccp_device_progress(session->device)
 * XXX name is bad
 *
 * \note Part of the device API.
 */
void sccp_session_progress(struct sccp_session *session);

/*!
 * \brief Transmit a message over the session.
 *
 * \note Part of the device API.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_session_transmit_msg(struct sccp_session *session, struct sccp_msg *msg);

/*!
 * \brief Return the remote (i.e. peer) IPv4 address of the session, as a char*.
 *
 * \note The returned pointer becomes invalid when the session reference count reach zero.
 */
const char *sccp_session_remote_addr_ch(const struct sccp_session *session);

/*!
 * \brief Return the local (i.e. sock) IPv4 address of the session.
 *
 * \note The returned pointer becomes invalid when the session reference count reach zero.
 */
const struct sockaddr_in *sccp_session_local_addr(const struct sccp_session *session);

#endif /* SCCP_SESSION_H_ */
