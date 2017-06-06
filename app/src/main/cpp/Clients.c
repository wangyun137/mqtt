#include "Clients.h"


/**
 * 通过clientId来比较Clients
 * @param a first integer value
 * @param b second integer value
 * @return boolean indicating whether a and b are equal
 */
int clientIdCompare(void* a, void* b) {
	Clients* client = (Clients*)a;
	return strcmp(client->clientID, (char*)b) == 0;
}


/**
 * 通过socket来比较Clients
 * @param a first integer value
 * @param b second integer value
 * @return boolean indicating whether a and b are equal
 */
int clientSocketCompare(void* a, void* b) {
	Clients* client = (Clients*)a;
	return client->net.socket == *(int*)b;
}
