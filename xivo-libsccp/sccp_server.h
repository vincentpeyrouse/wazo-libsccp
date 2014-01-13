#ifndef SCCP_SERVER_H_
#define SCCP_SERVER_H_

struct sccp_cfg;

/*!
 * \brief Initialize the server submodule.
 *
 * \note Must be called once before using anything else in the submodule.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_init(void);

/*!
 * \brief Free the resources associated to the server submodule.
 *
 * \note If the server is running, it will be stopped, like if sccp_server_stop
 *       had been called first.
 */
void sccp_server_destroy(void);

/*!
 * \brief Start the server.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_start(struct sccp_cfg *cfg);

/*!
 * \brief Stop the server and all the sessions.
 *
 * \note This is a blocking operation; when the function returns, the server
 *       is not running and no sessions are running either.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_stop(void);

/*!
 * \note This both reload the server configuration and the sessions configuration.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_reload_config(struct sccp_cfg *cfg);

#endif /* SCCP_SERVER_H_ */
