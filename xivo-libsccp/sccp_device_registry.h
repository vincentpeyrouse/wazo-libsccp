#ifndef SCCP_DEVICE_REGISTRY_H_
#define SCCP_DEVICE_REGISTRY_H_

struct sccp_device;
struct sccp_device_registry;
struct sccp_device_snapshot;

#define SCCP_DEVICE_REGISTRY_ALREADY 1

/*!
 * \brief Create a new device registry.
 *
 * \note The device registry is a thread safe container for devices.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device_registry *sccp_device_registry_create(void);

/*!
 * \brief Destroy the registry.
 */
void sccp_device_registry_destroy(struct sccp_device_registry *registry);

/*!
 * \brief Add a device to the registry.
 *
 * \retval 0 on success
 * \retval SCCP_DEVICE_REGISTRY_ALREADY if a device with the same same has already been added
 * \retval -1 on other failure
 */
int sccp_device_registry_add(struct sccp_device_registry *registry, struct sccp_device *device);

/*!
 * \brief Remove a device from the registry.
 */
void sccp_device_registry_remove(struct sccp_device_registry *registry, struct sccp_device *device);

/*!
 * \brief Take a snapshot of all the devices in the registry.
 *
 * XXX on success, *snapshots, must be freed, else you'll leak memory
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_device_registry_take_snapshots(struct sccp_device_registry *registry, struct sccp_device_snapshot **snapshots, size_t *n);

#endif /* SCCP_DEVICE_REGISTRY_H_ */
