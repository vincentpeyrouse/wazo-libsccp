#ifndef SCCP_TASK_H_
#define SCCP_TASK_H_

struct ast_heap;
struct sccp_session;
struct sccp_task_runner;

/*!
 * \brief Function type for session task callback
 */
typedef void (*sccp_task_cb)(struct sccp_session *session, void *data);

/*!
 * \brief A sccp session task
 */
struct sccp_task {
	sccp_task_cb callback;
	void *data;
};

/*!
 * \brief Helper function to create a sccp_task
 */
struct sccp_task sccp_task(sccp_task_cb callback, void *data);

/*!
 * \brief Create a new task runner.
 */
struct sccp_task_runner *sccp_task_runner_create(void);

void sccp_task_runner_destroy(struct sccp_task_runner *runner);

/*!
 * \brief Add/schedule a task
 *
 * \retval 0 on success
 * \retval non-zero on failure (i.e. task has not been added)
 *
 * \note If the task has already been added, the task is rescheduled
 *       and zero is returned.
 * \note It is safe to be called even in a task callback.
 * \note Not thread safe
 */
int sccp_task_runner_add(struct sccp_task_runner *runner, struct sccp_task task, int sec);

/*!
 * \brief Remove/unschedule a task
 *
 * \note It is not an error to remove a task that has not been added.
 * \note It is safe to be called even in a task callback.
 * \note Not thread safe
 */
void sccp_task_runner_remove(struct sccp_task_runner *runner, struct sccp_task task);

/*!
 * \brief Run the due tasks
 */
void sccp_task_runner_run(struct sccp_task_runner *runner, struct sccp_session *session);

/*!
 * Return the number of milliseconds before the next task.
 *
 * If the task is in the future, returns 0.
 *
 * If no task, return -1.
 */
int sccp_task_runner_next_ms(struct sccp_task_runner *runner);

#endif /* SCCP_TASK_H_ */
