#include <stdio.h>
#include <string.h>      
#include <stdlib.h> 
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>  //Header file for sleep(). man 3 sleep for details. 
#include <pthread.h>
#include <sched.h>
#include <math.h>

pthread_mutex_t lock;

struct Client_Pkt
{
	int32_t ID; // 4 bytes
	float x; // 4 bytes
	float y; // 4 bytes
	float z; // 4 bytes
	uint8_t msgtype; // 1 byte
 	uint8_t ACK; // 1 byte
	uint8_t pad2; // padding bytes
	uint8_t pad3;
	// other possible fields:
	// xrotation, yrotation, zrotation - 12 bytes
	// head clothing - 2 (?) bytes
	// body clothing / design - depends on implementation. 4 bytes?
	// pant color - 4 bytes (RGB(A))
	// number of boxes - 1 byte
	// animation state - 1 byte
	// 2 bytes to keep track of new parameters
	// brings theoretical total to 24 + 16 + 3 = 43 bytes 
};

struct Server_Pkt // i don't think we'll use a struct for messages from server to client; they'll be structured like a changelog
{
	uint8_t msgtype;
};

struct IDACK
{
	int32_t ID;
	uint8_t ACK;
	struct timeval time1;
};

struct State
{
	struct Client_Pkt* client_pkts;
	uint32_t num_of_clients;
};

struct Send_Struct
{
	uint32_t* clientaddr_array_len;
	uint32_t* clientaddr_array_filled;
	struct sockaddr_in* clientaddr_array;
	unsigned int* len_array;
	struct IDACK* IDACK_array;
	struct State* state_list;
	uint8_t* cur_state;
	uint32_t* cur_ID;
	int sockfd;
};

struct Recv_Struct
{
	uint32_t* clientaddr_array_len;
	uint32_t* clientaddr_array_filled;
	struct sockaddr_in* clientaddr_array;
	unsigned int* len_array;
	struct IDACK* IDACK_array;
	struct State* state_list;
	uint8_t* cur_state;
	uint32_t* cur_ID;
	int sockfd; 
};

enum Parse_States
{
	below_both_pcount,
	above_pcount_A,
	above_pcount_B,
	above_both
};

void remove_elem(void* list, uint32_t index, uint32_t len, uint32_t elem_size)
{
	if (index >= len)
	{
		printf("index is greater than len for remove_elem\n");
		return;
	}
	
	for (uint32_t m = index; m < len - 1; m += 1)
	{
		memcpy(list + m*elem_size, list + (m + 1)*elem_size, elem_size);
	}
	
	memset(list + (len - 1)*elem_size, 0, elem_size);
}

/*
	send parsing structure:
	
	0 1 2 3 4 5 6
	1 2 3 4 5 6

	0 1 2 3 4 5 6
	1 3 5

	write down that 0 has left, move array A's counter up 1
	get diff of prev and current 1, increment both counters by 1
	write down that 2 has left, inc A by 1
	diff of 3, inc both by 1
	write down that 4 has left, inc A by 1
	diff of 5, inc both by 1
	write down that 6 has left

	case 3: prev_state is shorter, people have left, people have joined
	1 3 5
	3 6 7 8 9
	
	1 2 3 4
	0 1 2 3 4

	write down that 1 has left, move A's counter up by 1
	get diff of 3, inc both by 1
	write down that 5 has left, move A's counter up by 1
	only 6 7 8 9 are left, so write down that they're new, increment appropriately
*/


// broadcast packet should contain list of ... client_pkts, i guess
void* send_func(void* void_in)
{
	struct Send_Struct* send_struct = (struct Send_Struct*)void_in;
	
	struct timeval time1;
	struct timeval time2;
	
	gettimeofday(&time1, NULL); // get initial time
	
	// x times a second, broadcast a packet containing the diff'd game info
	while(1)
	{
		// check if 1/20th of a second has passed
		gettimeofday(&time2, NULL);
		double start = (double) time1.tv_sec + (double) (time1.tv_usec)/1000000.0;
		double end = (double) time2.tv_sec + (double) (time2.tv_usec)/1000000.0;
		
		if (end - start < (1.0) || *(send_struct->clientaddr_array_filled) == 0)
			continue;
		else
			gettimeofday(&time1, NULL); // get new initial time
		
		pthread_mutex_lock(&lock);
		
		// look at rotating state list. for each client, send changelist of game info from previously acked state
		for (uint32_t i = 0; i < *(send_struct->clientaddr_array_filled); i += 1)
		{
			// first thing we want to do is look at this player's ID and find their corresponding ACK
			// then we diff that ACK's state against the current state, and send the player the changes.
			// if the player hasn't gotten a state yet, or this is the very first state, we send the entire game state.
			// (the above two are actually the same case)
			
			// tech: a player will always have the same index in the clientaddr and IDACK arrays
			
			uint8_t last_ack = send_struct->IDACK_array[i].ACK;
			int32_t ID = send_struct->IDACK_array[i].ID;
			
			// compose a diff btwn current state and previous one
			struct State dummy_state;
			dummy_state.client_pkts = NULL;
			dummy_state.num_of_clients = 0;
			struct State* prev_state = &(send_struct->state_list[last_ack]);
			if (last_ack == 0xF0) prev_state = &dummy_state;
			struct State* cur_state = &(send_struct->state_list[*(send_struct->cur_state)]);
			struct Client_Pkt* prev_clients = prev_state->client_pkts;
			struct Client_Pkt* cur_clients = cur_state->client_pkts;
			uint32_t prev_num = prev_state->num_of_clients;
			uint32_t cur_num = cur_state->num_of_clients;
			
			int prev_counter = 0;
			int cur_counter = 0;
							
			enum Parse_States parse_state = below_both_pcount;
			
			uint8_t* message_buffer = NULL;
			unsigned int buffer_index = 2;
			unsigned int x = 4;
			unsigned int message_buffer_size = *(send_struct->clientaddr_array_filled) * (sizeof(struct Client_Pkt) + x) + prev_num * (4 + 1) + 1 + 1;
			// each message will be at most (number of players) * (size of player packet + x(byte indicator for each field)) + prev * (4 + 1) (for indicating deleted IDs and corresponding indicator) + 1 (for message type) + 1 (for ACK num)
			if ((message_buffer = malloc(message_buffer_size)) == NULL)
			{
				printf("error with message_buffer malloc in send_func\n");
				return NULL;
			}
			
			message_buffer[0] = 0x02; // indicating that this is an update message
			message_buffer[1] = *(send_struct->cur_state);
			
			while (1)
			{
				switch (parse_state)
				{
					case below_both_pcount:
						if (prev_counter == prev_num)
						{
							parse_state = above_pcount_A;
							break;
						}
						else if (cur_counter == cur_num)
						{
							parse_state = above_pcount_B;
							break;
						}
						
						if (cur_clients[cur_counter].ID == prev_clients[prev_counter].ID)
						{
							// compose diff
							// increment both by 1
							message_buffer[buffer_index++] = 'I'; // to indicate ID
							memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].ID), sizeof(int32_t));
							buffer_index += 4;
							
							if (cur_clients[cur_counter].x != prev_clients[prev_counter].x)
							{
								message_buffer[buffer_index++] = 'x'; // to indicate new x
								memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].x), sizeof(float));
								buffer_index += 4;
							}
							if (cur_clients[cur_counter].y != prev_clients[prev_counter].y)
							{
								message_buffer[buffer_index++] = 'y'; // to indicate new y
								memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].y), sizeof(float));
								buffer_index += 4;
							}
							if (cur_clients[cur_counter].z != prev_clients[prev_counter].z)
							{
								message_buffer[buffer_index++] = 'z'; // to indicate new z
								memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].z), sizeof(float));
								buffer_index += 4;
							}
						
							cur_counter += 1;
							prev_counter += 1;
						}
						else if (cur_clients[cur_counter].ID < prev_clients[prev_counter].ID)
						{
							// increment cur_counter by 1
							// this case shouldn't actually happen. see the below example:
							// 1 2 3 4
							// 0 1 2 3 4
							// the way we're handling arrays, we shouldn't have a 0 where there used to be a 1.
							// come back here if you need to, later
							cur_counter += 1;
						}
						else
						{
							// increment prev_counter by 1
							// case like
							// 1 2 3 4
							// 2 3 4
							// where ID 1 dropped off and is no longer there
							message_buffer[buffer_index++] = 'i'; // to indicate removed ID
							memcpy(message_buffer + buffer_index, &(prev_clients[prev_counter].ID), sizeof(int32_t));
							buffer_index += 4;

							prev_counter += 1;
						}
							
						break;
					case above_pcount_A:
						if (cur_counter == cur_num)
						{
							parse_state = above_both;
							break;
						}
						
						// push cur_clients onto the update and inc by 1
						
						message_buffer[buffer_index++] = 'I'; // to indicate ID
						memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].ID), sizeof(int32_t));
						buffer_index += 4;
						message_buffer[buffer_index++] = 'x'; // to indicate new x
						memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].x), sizeof(float));
						buffer_index += 4;
						message_buffer[buffer_index++] = 'y'; // to indicate new y
						memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].y), sizeof(float));
						buffer_index += 4;
						message_buffer[buffer_index++] = 'z'; // to indicate new z
						memcpy(message_buffer + buffer_index, &(cur_clients[cur_counter].z), sizeof(float));
						buffer_index += 4;
						
						cur_counter += 1;
						break;
					case above_pcount_B:
						if (prev_counter == prev_num)
						{
							parse_state = above_both;
							break;
						}
						
						// push prev_clients onto the update and inc by 1
						message_buffer[buffer_index++] = 'i'; // to indicate removed ID
						memcpy(message_buffer + buffer_index, &(prev_clients[prev_counter].ID), sizeof(int32_t));
						buffer_index += 4;							
						
						prev_counter += 1;
						break;
					case above_both:
						break;
				}
				
				if (parse_state == above_both)
					break;
			}
			
			// send changes in 1492-byte chunks
			
			unsigned int num_of_bytes = buffer_index;
			
			// TODO: this packet structure will break if changes exceeed 1492 bytes. I'll come back to this later,
			// when the 1 and 2-player cases work
			
			while (num_of_bytes > 0)
			{
				unsigned int local_num = 1492;
				if (num_of_bytes < 1492)
					local_num = num_of_bytes;
				
				// Debug: print the packet
				for (int i = 0; i < local_num; i++)
				{
					printf("%x ", message_buffer[i]);
				}
				printf("\n");
				
				sendto(send_struct->sockfd, message_buffer, local_num, 0,
					  (struct sockaddr*)&(send_struct->clientaddr_array[i]), send_struct->len_array[i]); // send the message
				
				num_of_bytes -= local_num;
			}

			free(message_buffer);
		}
		
		uint8_t* cur_state_num = send_struct->cur_state;
		uint8_t next_state_num = (*cur_state_num + 1) % 202;
		
		send_struct->state_list[next_state_num].num_of_clients = send_struct->state_list[*cur_state_num].num_of_clients; 
		
		if (send_struct->state_list[next_state_num].client_pkts != NULL)
		{
			free(send_struct->state_list[next_state_num].client_pkts);
			send_struct->state_list[next_state_num].client_pkts = NULL;
		}
		
		if ((send_struct->state_list[next_state_num].client_pkts = malloc(sizeof(struct Client_Pkt) * send_struct->state_list[*cur_state_num].num_of_clients)) == NULL)
		{
			printf("error with client_pkts malloc in send_func\n");
			return NULL;
		}
			
		memcpy(send_struct->state_list[next_state_num].client_pkts,
			   send_struct->state_list[*cur_state_num].client_pkts,
			   send_struct->state_list[*cur_state_num].num_of_clients * sizeof(struct Client_Pkt));
		
		*cur_state_num = next_state_num;
		
		pthread_mutex_unlock(&lock);
		
		gettimeofday(&time1, NULL);
	}

	return NULL;

}

void* recv_func(void* void_in)
{
	struct Recv_Struct* recv_struct = (struct Recv_Struct*)void_in;
	
	uint8_t* msg_buf;
	unsigned int msg_len = sizeof(struct Client_Pkt);
	if ((msg_buf = malloc(sizeof(struct Client_Pkt))) == NULL)
	{
		printf("error with msg_buf malloc in recv_func\n");
		return NULL;
	}
	
	// here, get establishment packets, ACKs from each clientaddr
	while(1)
	{
		pthread_mutex_lock(&lock);
		
		struct sockaddr_in local_clientaddr;
		memset(&local_clientaddr, 0, sizeof(struct sockaddr_in));
		unsigned int len = sizeof(local_clientaddr);

		unsigned int bytes_read = recvfrom(recv_struct->sockfd, msg_buf, msg_len, 0, (struct sockaddr*)&local_clientaddr, &len);	

		if (bytes_read == sizeof(struct Client_Pkt))
		{
			struct Client_Pkt* cpkt = (struct Client_Pkt*)msg_buf;
			
			if (cpkt->msgtype == 0x00) // establishment packet. add info to list if it hasn't already been added, send back ID
			{
				uint8_t found_clientaddr = 0;
				int32_t found_clientaddr_ID = -1;
				for (uint32_t i = 0; i < *(recv_struct->clientaddr_array_filled); i++)
				{
					short rs_short = recv_struct->clientaddr_array[i].sin_family;
					short lc_short = local_clientaddr.sin_family;
					unsigned short rs_ushort = recv_struct->clientaddr_array[i].sin_port;
					unsigned short lc_ushort = local_clientaddr.sin_port;
					unsigned long rs_ulong = recv_struct->clientaddr_array[i].sin_addr.s_addr;
					unsigned long lc_ulong = local_clientaddr.sin_addr.s_addr;
					if (rs_short == lc_short && rs_ushort == lc_ushort && rs_ulong == lc_ulong) // we already have the clientaddr in the list
					{
						found_clientaddr = 1;
						found_clientaddr_ID = recv_struct->IDACK_array[i].ID;
						recv_struct->len_array[i] = len;
						break;
					}
				}
				
				if (found_clientaddr == 0) // if the client isn't already in the list . . .
				{
					if (*(recv_struct->clientaddr_array_filled) < *(recv_struct->clientaddr_array_len)) // check if we have the max number players. if not, put player on list
					{
						recv_struct->IDACK_array[*(recv_struct->clientaddr_array_filled)].ID = *(recv_struct->cur_ID);
						recv_struct->IDACK_array[*(recv_struct->clientaddr_array_filled)].ACK = 0xF0;
						gettimeofday(&(recv_struct->IDACK_array[*(recv_struct->clientaddr_array_filled)].time1), NULL);
						memcpy(&(recv_struct->clientaddr_array[*(recv_struct->clientaddr_array_filled)]), &local_clientaddr, sizeof(struct sockaddr_in));			
						recv_struct->len_array[*(recv_struct->clientaddr_array_filled)] = len;

						uint32_t cc = recv_struct->state_list[*(recv_struct->cur_state)].num_of_clients;
						if (cc == 0)
						{
							if ((recv_struct->state_list[*(recv_struct->cur_state)].client_pkts = malloc(sizeof(struct Client_Pkt))) == NULL)
							{
								printf("error with client_pkts malloc in recv_func\n");
								exit(2);
							}
						}
						else 
						{
							if ((recv_struct->state_list[*(recv_struct->cur_state)].client_pkts = realloc(recv_struct->state_list[*(recv_struct->cur_state)].client_pkts, sizeof(struct Client_Pkt) * (cc + 1))) == NULL)
							{
								printf("error with client_pkts realloc in recv_func\n");
								exit(2);
							}
						}
						recv_struct->state_list[*(recv_struct->cur_state)].num_of_clients += 1;
						memcpy(recv_struct->state_list[*(recv_struct->cur_state)].client_pkts + cc, cpkt, sizeof(struct Client_Pkt)); // copy the contents of this packet into the state
						
						uint8_t ID_buf[9] = {0}; // 5-byte buffer for sending 1. message type and 2. ID number
						ID_buf[0] = 0x01; // indicating that this is an ID message.
						memcpy(ID_buf + 1, recv_struct->cur_ID, 4); // copy the literal bytes of the ID from recv_struct->clientaddr_array_filled into our message
						memcpy(ID_buf + 5, recv_struct->clientaddr_array_len, 4); // send the max num of players, too
						sendto(recv_struct->sockfd, ID_buf, 9, 0, (struct sockaddr*)&local_clientaddr, len); // send the message
						printf("sent initializer packet. ID is %d\n", *recv_struct->cur_ID);
						
						*(recv_struct->clientaddr_array_filled) += 1;
						*(recv_struct->cur_ID) += 1;
					}
					else 				// if we have max players, then send back error packet
					{
						uint8_t ID_buf[1]; // 1-byte buffer for error code
						ID_buf[0] = 0xFF;
						sendto(recv_struct->sockfd, ID_buf, 1, 0, (struct sockaddr*)&local_clientaddr, len); // send the message
					}
				}
				else // just send back the player's ID
				{
					// same comments as above, only change is that we get ID from found_clientaddr_ID instead of recv_struct->clientaddr_array_filled
					uint8_t ID_buf[9] = {0};
					ID_buf[0] = 0x01;
					memcpy(ID_buf + 1, &found_clientaddr_ID, 4);
					memcpy(ID_buf + 5, recv_struct->clientaddr_array_len, 4); // send the max num of players, too
					sendto(recv_struct->sockfd, ID_buf, 9, 0, (struct sockaddr*)&local_clientaddr, len);
					printf("sent duplicate ID packet. ID is %d\n", found_clientaddr_ID);
				}
			}
			else if (cpkt->msgtype == 0x01) // regular packet. update info in list and get ACK number, update timeout timer. don't have to send back anything, hopefully
			{
				for (uint32_t i = 0; i < *(recv_struct->clientaddr_array_filled); i++)
				{
					if (cpkt->ID == recv_struct->IDACK_array[i].ID)
					{
						recv_struct->len_array[i] = len;
						recv_struct->IDACK_array[i].ACK = cpkt->ACK;
						memcpy(recv_struct->state_list[*(recv_struct->cur_state)].client_pkts + i, cpkt, sizeof(struct Client_Pkt));
						printf("received %f, %f, %f\n", cpkt->x, cpkt->y, cpkt->z);
						
						gettimeofday(&(recv_struct->IDACK_array[i].time1), NULL);
						continue;
					}
				}			
			}
			else if (cpkt->msgtype == 0x02) // exit packet. remove player from IDACK and clientaddr lists
			{
				
			}
		} 
		else 
		{
			pthread_mutex_unlock(&lock);
			continue;
		}
		pthread_mutex_unlock(&lock);
		
		usleep(2000);
	}

	return NULL;
}

void* cleanup_func(void* void_in)
{
	struct Recv_Struct* recv_struct = (struct Recv_Struct*)void_in;
	
	struct timeval thread_time_1;
	struct timeval thread_time_2;
	gettimeofday(&thread_time_1, NULL);
	while(1)
	{
		gettimeofday(&thread_time_2, NULL);
		double start = (double) thread_time_1.tv_sec + (double) (thread_time_1.tv_usec)/1000000.0;
		double end = (double) thread_time_2.tv_sec + (double) (thread_time_2.tv_usec)/1000000.0;
		if ((end - start) < 1.0) // not yet time to run
			sched_yield();
		else
		{
			for (uint32_t i = 0; i < *(recv_struct->clientaddr_array_filled); i++)
			{
				uint32_t cf = *(recv_struct->clientaddr_array_filled);
				struct timeval time1 = recv_struct->IDACK_array[cf].time1;
				start = (double) time1.tv_sec + (double) (time1.tv_usec)/1000000.0;
				if ((end - start) > 10.0) // this player has timed out
				{
					remove_elem(recv_struct->IDACK_array, i, cf, sizeof(struct IDACK));
					remove_elem(recv_struct->clientaddr_array, i, cf, sizeof(struct sockaddr_in));
					remove_elem(recv_struct->state_list[*(recv_struct->cur_state)].client_pkts, i, cf, sizeof(struct Client_Pkt));
					*(recv_struct->clientaddr_array_filled) -= 1;
					i -= 1;
				}
			}
		}
		
		gettimeofday(&thread_time_1, NULL);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
		return 1;
			
	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0) ) < 0)
	{
		printf("cannot socket()");
		exit(1);
	}
	
	uint32_t clientaddr_array_len = atoi(argv[2]);
	uint32_t clientaddr_array_filled = 0;
	struct sockaddr_in* clientaddr_array;
	if ((clientaddr_array = malloc(sizeof(struct sockaddr_in) * clientaddr_array_len)) == NULL)
	{
		printf("malloc of clientaddr_array broke, exiting\n");
		return 1;
	}
	
	memset(clientaddr_array, 0, sizeof(struct sockaddr_in) * clientaddr_array_len);

	unsigned int* len_array;
	if ((len_array = malloc(sizeof(uint32_t) * clientaddr_array_len)) == NULL)
	{
		printf("malloc of len_array broke, exiting\n");
		return 1;
	}
	
	memset(len_array, 0, sizeof(unsigned int) * clientaddr_array_len);
		
	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(struct sockaddr_in));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(atoi(argv[1]));
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	if (bind(sockfd, (const struct sockaddr*)&servaddr, sizeof(struct sockaddr_in)))
	{
		printf("cannot bind()");
		exit(1);
	}		
				
	// ideally, run a different thread for each client. thread handles message sending to the client 
	// (can get ACKs, send only diff data [I don't think sending diff'd memory chunks like quake 3 is 
	// going to work on modern systems, but doing it manually might not be terrible,
	// we have enough processing power] and closes after 10 second timeout or something
	
	// actually, don't do separate threads for now, because I really don't know how that would work with UDP!!
	// just keep a list of clientaddrs, send conglomarate packet to each
	
	uint8_t cur_state = 0;
	uint32_t cur_ID = 0;
	
	struct IDACK* IDACK_array;
	if ((IDACK_array = malloc(sizeof(struct IDACK) * clientaddr_array_len)) == NULL)
	{
		printf("malloc of IDACK_array broke, exiting\n");
		return 1;
	}
	
	struct State* state_list;
	if ((state_list = malloc(sizeof(struct State) * 201)) == NULL)
	{
		printf("malloc of state_list broke, exiting\n");
		return 1;
	}
		
	for (int m = 0; m < 201; m++)
	{
		state_list[m].client_pkts = NULL;
		state_list[m].num_of_clients = 0;
	}
	
	struct Recv_Struct recv_struct;
	recv_struct.clientaddr_array_len = &clientaddr_array_len;
	recv_struct.clientaddr_array_filled = &clientaddr_array_filled;
	recv_struct.clientaddr_array = clientaddr_array;
	recv_struct.len_array = len_array;
	recv_struct.IDACK_array = IDACK_array;
	recv_struct.state_list = state_list;
	recv_struct.cur_state = &cur_state;
	recv_struct.cur_ID = &cur_ID;
	recv_struct.sockfd = sockfd;	

    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        printf("\n mutex init failed\n");
        return 1;
    }

    pthread_t recv_thread_id; 
    pthread_t send_thread_id; 
	pthread_create(&recv_thread_id, NULL, recv_func, (void*)(&recv_struct));
	pthread_create(&send_thread_id, NULL, send_func, (void*)(&recv_struct)); 
    pthread_join(send_thread_id, NULL); 
	pthread_join(recv_thread_id, NULL); 
	pthread_mutex_destroy(&lock);
	
	return 0;
}

