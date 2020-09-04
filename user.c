#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/errno.h>
#include <errno.h>
#include <ctype.h>

#define ENOUGH_FOR_MEX 100
#define ENOUGH  30 //min 24 bytes needed for mknod command 

#define SET_SEND_TIMEOUT _IOW(1,1, int *)
#define SET_RECV_TIMEOUT _IOW(1, 2,int *)
#define REVOKE_DELAYED_MEX _IO(1,3)


/**********************
  Il main riceve come input opzionale il path del nodo di I/O da creare, il major e poi il minor number.

  Ad es. alla compilazione esegui:
  >>sudo ./user /dev/my-dev 241 10
  {qui 241 è il major e 10 è il minor number scelto }


NB: Eseguire sempre con sudo!!
 ***********************/





int main(int argc, char** argv){
	int i;
	int jiffies;
	static int fd;
	int major;
	int minor;
	char *path;
	int   length;
	char op[10]; //read|write|ioctl
	char mex[ENOUGH_FOR_MEX];
	char temp[ENOUGH];
	static bool ioctl_command;

	/*il programma può ricevere un input se si vuole creare un nodo I/O oppure niente se si vuole solo scrivere\leggere sui nodi*/
	if(argc>4){
		printf("usage: prog [pathname major minors] \n");
		return -1;
	}

	//crea il nodo di I/O
	if(argc==4){
		path = argv[1];
		major = strtol(argv[2],NULL,10);
		minor = strtol(argv[3],NULL,10);



		memset(temp,0,ENOUGH);
		//controllo se device file da creare già esiste
		sprintf(temp,"%s",path);
		if( access( temp, F_OK ) == 0) {
			//il file esiste già...
			printf(" File %s already exists. Retry to execute with another name\n",temp);
			return -EEXIST ;
		}

		sprintf(temp,"mknod %s c %d %i -m 0770\n",path,major,minor);
		// esecuzione di comando da shell
		system(temp);            
		printf("Device file %s creato\n", path);		
		//per eventuali errori sul minor number passato occorre aspettare l'open()

	}



	while (1){
		memset(mex,0,ENOUGH_FOR_MEX);
		memset(temp,0,ENOUGH);
		printf("Choose an operation: [read|write|ioctl|flush] \n");


		//qui si prende l'operazione (read, write, ioctl o flush)
		read(STDIN_FILENO, op, sizeof(op));


		if(strncmp(op,"read",strlen("read"))==0){
			printf("Insert the size of the message to be readed: \n");
retry_size_read:

			//il secondo input conterrà il numero max di bytes da leggere
			read(STDIN_FILENO,temp,sizeof(temp));			

			for (i=0;i<(int)strlen(temp)-1; i++)
				if (!isdigit(temp[i]))
				{
					printf("Invalid input, insert a number.\n");
					memset(temp,0,sizeof(temp));
					goto retry_size_read;
				}

			length=strtol(temp,NULL,10);

			if(!ioctl_command){
				printf("Type name of device file to read: \n");
retry_file_read:
				memset(temp,0,ENOUGH);
				//il terzo input conterrà il nome del file da leggere
				i=read(STDIN_FILENO,temp,sizeof(temp));

				path=(char*)malloc(i-1);          
				strncpy(path,temp,i-1);
				fd = open(path,O_RDWR);

				if( fd  < 0) {
					printf("Error opening the file: %s\nRetry:\n", strerror(errno));
					goto retry_file_read ;
				}
			}//se ioctl_command==true allora non serve specificare il file

			//ora si manda invocazione di lettura  
			length= read(fd,mex,length);
			if(length>0){
				printf("Got message:  %s \n",mex);
			}	
			if (length<=0){
				printf("Got no message \n");}

		}//fine read



		else if (strncmp(op,"write",strlen("write"))==0){

			printf("Insert the message to be delivered: \n");
			//qui si passa il messaggio
			fgets(temp,ENOUGH,stdin);

			if ((ENOUGH_FOR_MEX-strlen(mex))<strlen(temp)){
				printf("Message written is too long, modify compiled variable ENOUGH_TO_MEX\n");
				return -1;
			}

			//append del messaggio in mex
			strncat(mex,temp,strlen(temp)); //If  temp contains n or more bytes, strncat() writes n+1 bytes to mex (n from temp plus the terminating null byte).
			if(!ioctl_command){

				//ora si passa il nome del file
				printf("Type name of device file to write: \n");
retry_file_write:
				memset(temp,0,ENOUGH);
				//il terzo input conterrà il nome del file da leggere
				i=read(STDIN_FILENO,temp,sizeof(temp));

				path=(char*)malloc(i-1);          
				strncpy(path,temp,i-1);
				if(!ioctl_command){
					fd = open(path,O_RDWR);
				}

				if( fd  < 0) {
					printf("Error opening the file: %s\nRetry:\n", strerror(errno));
					goto retry_file_write ;
				}
			}

			write(fd,mex,strlen(mex));
			//per sapere se la scrittura è andata a buon fine occorrerà vedere dmesg

		}  



		else if (strncmp(op,"ioctl",strlen("ioctl"))==0){


			printf("Insert a number: 1=SET_SEND_TIMEOUT,2=SET_RECV_TIMEOUT,3=REVOKE_DELAYED_MESSAGES \n");

retry_ioctl:
			//qui si passa il messaggio
			fgets(temp,ENOUGH,stdin);

			length=strtol(temp,NULL,10);
			if (length<=0||length>3)
			{
				printf("Invalid input, insert a number.\n");
				memset(temp,0,sizeof(temp));
				goto retry_ioctl;
			}
			memset(temp,0,sizeof(temp));
			if(length!=3){
				printf("Insert number of jiffies:\n");

retry_jiffies:
				//qui si passa il numero di jiffies
				fgets(temp,ENOUGH,stdin);
				jiffies=strtol(temp,NULL,10);
				if (jiffies<0)
				{
					printf("Invalid input, insert a number.\n");
					memset(temp,0,sizeof(temp));
					goto retry_jiffies;
				}

			}
			//si richiede il percorso del file solo se non è stato invocato in precedenza un ioctl (perchè altrimenti abbiamo già il fd)
			if(!ioctl_command){
				//ora si passa il nome del file
				printf("Type name of device file: \n");
retry_file_ioctl:
				memset(temp,0,ENOUGH);

				i=read(STDIN_FILENO,temp,sizeof(temp));

				path=(char*)malloc(i-1);          
				strncpy(path,temp,i-1);
				fd = open(path,O_RDWR);

				if( fd  < 0) {
					printf("Error opening the file: %s\nRetry:\n", strerror(errno));
					goto retry_file_ioctl ;
				}
			}
			switch(length){

				case 1:      
					ioctl(fd,SET_SEND_TIMEOUT,&jiffies);
					printf("SET_SEND_TIMEOUT enabled\n");

					//l'istruzione seguente serve lavorare su questa sessione e non aprirne altre
					ioctl_command=true;
					break;

				case 2:      
					ioctl(fd,SET_RECV_TIMEOUT,&jiffies);
					printf("SET_RECV_TIMEOUT enabled\n");
					ioctl_command=true;
					break;

				case 3:      
					ioctl(fd,REVOKE_DELAYED_MEX);
					printf("REVOKE_DELAYED_MEX enabled\n");
					ioctl_command=true;
					break;


			}

		}
		else if (strncmp(op,"flush",strlen("flush"))==0){


			if (fd==0){
				printf("Type name of device file: \n");
retry_file_flush:
				memset(temp,0,ENOUGH);
				//il nome del file da flushare
				i=read(STDIN_FILENO,temp,sizeof(temp));

				path=(char*)malloc(i-1);          
				strncpy(path,temp,i-1);
				fd = open(path,O_RDWR);

				if( fd  < 0) {
					printf("Error opening the file: %s\nRetry:\n", strerror(errno));
					goto retry_file_flush ;
				}
			}
			close(fd);
			ioctl_command=false;//per poter aprire un file esplicitamente
			printf("Device file flushato\n");	
			fd=0;
		}

		else {
			printf("invalid input, insert: [read|write|ioctl|flush] \n");

		}	


	}



}
