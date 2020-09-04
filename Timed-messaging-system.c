#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>		/* For pid types */
#include <linux/version.h>	/* For LINUX_VERSION_CODE */
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simone Mesiano Laureani");

#define MODNAME "TIME MESSAGING"

#define SCALING (1000)  // please take this value from CONFIG_HZ in your kernel config file 

#define DEVICE_NAME "my-dev"  /* Device file name in /dev/ - not mandatory  */
#define AUDIT if(0)//1=print printk

/* numero di minor da gestire*/
#define MINORS 4 //di default saranno da 0 a 7

#define SET_SEND_TIMEOUT _IOW(1,1, int *)
#define SET_RECV_TIMEOUT _IOW(1, 2,int *)
#define REVOKE_DELAYED_MEX _IO(1,3)

static int MAX_MESSAGE_SIZE = 64;/* this can be configured at run time via the sys file system  */
module_param(MAX_MESSAGE_SIZE,int,0660);

static int MAX_STORAGE_SIZE =100; /* this can be configured at run time via the sys file system  */
module_param(MAX_STORAGE_SIZE,int,0660);


/*--------------------------------------------------------------------*/

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static int dev_flush(struct file *filp,fl_owner_t id);




static int Major;            /* Major number assigned to broadcast device driver */

/*
   Di seguito ci sono delle macro per ottenere i giusti valori dei parametri a seconda della versione del kernel
   */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)	MAJOR(session->f_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)	MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_dentry->d_inode->i_rdev)
#endif



typedef struct message_node{
	char *buffer;
	size_t  valid;
	int minor;
	struct mutex object_being_reading;
	struct message_node *next_mex;
	struct work_struct the_work;
	struct delayed_work delayed_work;
} message_node;

typedef struct delayed_write_node{ //usato per la REVOKE_DELAYED_MEX
	struct mutex object_being_writing;
	struct delayed_write_node *next_delayed_work;
	struct delayed_work *del_work;
} delayed_write_node;

typedef struct session_ioctl_op{
	int jiffies_send;
	int jiffies_recv;
	bool set_send_timeout; 
	bool set_recv_timeout;
} session_ioctl_op;


message_node *works[MINORS]; 

static wait_queue_head_t wait_queue;  //settato in init_module

struct workqueue_struct *wq_Write[MINORS];   //settato in init_module

bool flushed[MINORS];   //settato da ioctl

delayed_write_node *delayed_writes[MINORS];//settato alle singole delayed write e utilizzato per cancellare i lavori pendenti alla ricezione di REVOKE_DELAYED_MEX



static int dev_open(struct inode *inode, struct file *file) {

	int minor;
	minor = get_minor(file); 

	if(minor >= MINORS){
		return -ENODEV;
	}


	return 0;

}


static int dev_release(struct inode *inode, struct file *file) {

	int minor;
	minor = get_minor(file);

	//device closed by default nop
	return 0;

}




void deferred_Write( void *data){

	int my_minor;
	ssize_t storage_used;
	message_node *cursor;

	storage_used=0;

	AUDIT{
		printk("WRITE occurred: minor %d\n",(container_of(data,message_node,the_work))->minor);
	}

	my_minor = (container_of(data,message_node,the_work))->minor;

	//guarda se nella prima sturct globale è stato scritto il buffer
	if  ((works[my_minor]->buffer) == NULL){

		(works[my_minor])->buffer=(container_of(data,message_node,the_work))->buffer;
		(works[my_minor])->valid=(container_of(data,message_node,the_work))->valid;
	}
	else{

		cursor= works[my_minor];

		while(cursor->next_mex != NULL ){
			//conta i bytes utilizzati dai vecchi messaggi
			storage_used += cursor->valid;
			cursor = cursor->next_mex;
		}
		storage_used += cursor->valid;

		//controllo dello storage utilizzato finora
		if((MAX_STORAGE_SIZE-storage_used) <(container_of(data,message_node,the_work)->valid)){
			printk(KERN_WARNING "%s: Write requested on minor %d but maximum size is already met, then the post is failed... \n",MODNAME,my_minor);       
			return;
		}
		//if(container_of(data,message_node,the_work))->valid
		cursor->next_mex=(container_of(data,message_node,the_work));

	}
	AUDIT{
		printk("Written message with size bytes= %d\n",(int)(container_of(data,message_node,the_work))->valid);}
}


// implementazione è defferred_wirte, cambia solo che al posto di work_struct si ha delayed_work nei container_of
void deferred_delayed_Write( void *data) {

	int my_minor;
	ssize_t storage_used;
	message_node *cursor;

	storage_used=0;
	AUDIT{
		printk("WRITE delayed occurred: minor %d\n",(container_of(data,message_node,delayed_work))->minor);}

	my_minor = (container_of(data,message_node,delayed_work))->minor;

	//guarda se nella prima sturct globale è stato scritto il buffer
	if  ((works[my_minor]->buffer) == NULL){


		(works[my_minor])->buffer=(container_of(data,message_node,delayed_work))->buffer;
		(works[my_minor])->valid=(container_of(data,message_node,delayed_work))->valid;
	}
	else{

		cursor= works[my_minor];

		while(cursor->next_mex != NULL ){
			//conta i bytes utilizzati dai vecchi messaggi
			storage_used += cursor->valid;
			cursor = cursor->next_mex;
		}
		storage_used += cursor->valid;
		//controllo dello storage utilizzato finora
		if((MAX_STORAGE_SIZE-storage_used) <(container_of(data,message_node,delayed_work)->valid)){
			printk(KERN_WARNING "%s: The request to deferred write message: %s \n on minor %d is failed because maximum size is already achieved.\n",MODNAME,(container_of(data,message_node,delayed_work))->buffer,my_minor);       
			return;
		}

		cursor->next_mex=(container_of(data,message_node,delayed_work));

	}
	AUDIT{
		printk("Written delayed message with size bytes= %d\n",(int)(container_of(data,message_node,delayed_work))->valid);}
}






static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {


	message_node *my_work;
	int my_minor;
	int ret;
	session_ioctl_op *p; 
	delayed_write_node *cursor;

	my_minor = get_minor(filp);
	flushed[my_minor]=false;//serve per poter scrivere dopo che qualcuno ha chiamato flush in precedenza
	if (len> MAX_MESSAGE_SIZE){
		return -EFBIG;
	}

	if (len== 0){
		return -1;
	}

	my_work = kzalloc(sizeof(message_node),GFP_ATOMIC);

	if (my_work == NULL) {
		printk("%s: memory allocation for work is failed\n",MODNAME);
		return -1;
	}

	my_work->minor = my_minor; 
	my_work->valid= len;
	my_work->buffer = kzalloc(len,GFP_ATOMIC);

	if(my_work->buffer== NULL){
		printk("%s: memory allocation for taken message is failed\n",MODNAME);
		return -1;
	}

	ret = copy_from_user(my_work->buffer,buff,len);
	if(ret){
		printk("%s: Some byte couldn't be copied\n",MODNAME);
		return -1;

	}


	p= filp->private_data;
	if(((filp->private_data)!=NULL)&&(p->set_send_timeout)){
		//deferred work
		INIT_DELAYED_WORK(&(my_work->delayed_work),(void *)deferred_delayed_Write);
		AUDIT{
			printk("Write deferred impostata\n");}

		if(!queue_delayed_work(wq_Write[my_minor], &(my_work->delayed_work),p->jiffies_send)){
			printk("ERROR queueing the work on workqueue\n");
		}

		//scrittura indirizzo delayed_work creato in una lista scandita in futuro per cancellare i job pending alla REVOKE
		if( (delayed_writes[my_minor]->del_work==NULL)&&(mutex_trylock(&(delayed_writes[my_minor]->object_being_writing)))){
			delayed_writes[my_minor]->del_work=&(my_work->delayed_work);
			mutex_unlock(&(delayed_writes[my_minor]->object_being_writing));
		}
		else{
			cursor= delayed_writes[my_minor];
			while( (cursor->next_delayed_work!=NULL) || (!(mutex_trylock(&(cursor->object_being_writing))))){

				cursor=cursor->next_delayed_work;
			}
			cursor->next_delayed_work=kzalloc(sizeof(delayed_write_node),GFP_ATOMIC);
			cursor->next_delayed_work->del_work=&(my_work->delayed_work);
			mutex_unlock(&(cursor->object_being_writing));
		}


	} 
	else{
		INIT_WORK(&(my_work->the_work),(void *)deferred_Write);
		if(!queue_work(wq_Write[my_minor], &(my_work->the_work))){
			printk("ERROR queueing the work on workqueue\n");
		}
	}


	return len;

}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {
	message_node *cursor;
	int my_minor;
	int ret;
	int res;
	session_ioctl_op *p;

	my_minor = get_minor(filp);
	flushed[my_minor]=false;//serve per poter leggere dopo che qualcuno ha chiamato flush

	p=filp->private_data;

	if(((filp->private_data)!=NULL)&&(p->set_recv_timeout)){
		AUDIT{
			printk("%s:Set up of deferred read with jiffies %d\n", MODNAME,p->jiffies_recv);}
		//deferred read
		res=wait_event_interruptible_timeout(wait_queue, flushed[my_minor]==true, p->jiffies_recv);

	}



	//cerca se ci sono dati nella prima struct e se il mutex è libero
	if(((works[my_minor])->valid !=(ssize_t) 0) && (mutex_trylock(&(works[my_minor]->object_being_reading)))){

		if(len<(works[my_minor])->valid){
			ret = copy_to_user(buff,(works[my_minor])->buffer,len);
		}
		else {
			ret = copy_to_user(buff,(works[my_minor])->buffer,(works[my_minor]->valid));
		}
		//ora si previene una eventuale lettura su questa struct
		works[my_minor]->valid= 0;
		mutex_unlock(&(works[my_minor]->object_being_reading));         

	}

	else{  

		cursor = (works[my_minor])->next_mex;

		while((cursor!=NULL)&&((cursor->valid ==(ssize_t) 0)||(!(mutex_trylock(&(cursor->object_being_reading)))))) {
			cursor = cursor->next_mex;

		}
		if(cursor==NULL){
			AUDIT{
				printk("No message founded to read\n");}
			return -ENOMSG;
		}
		if(len<(cursor)->valid){
			ret = copy_to_user(buff,cursor->buffer ,len);
		}
		else{
			ret = copy_to_user(buff,cursor->buffer ,(cursor)->valid);
		}
		//ora si previene una eventuale lettura su questa struct
		cursor->valid= 0;
		//rimozione nodo letto
		/**/      if(cursor->next_mex!=NULL){
			/**/    (works[my_minor])->next_mex=cursor->next_mex;
			/**/     }
		mutex_unlock(&(cursor->object_being_reading));
	}


	return len -ret;

}


static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param) {
	int my_minor;   
	int *time;
	session_ioctl_op *p;
	delayed_write_node *point;


	my_minor = get_minor(filp);



	if(command==SET_SEND_TIMEOUT){

		//controllo se è il primo comando ioctl ricevuto
		if(filp->private_data==NULL){
			filp->private_data= kzalloc(sizeof(session_ioctl_op),GFP_ATOMIC);
		}
		p=filp->private_data;
		time = (int *)param;
		p->jiffies_send=*time;
		p->set_send_timeout= true;

		return 0;
	}



	else if(command==SET_RECV_TIMEOUT){



		//controllo se è il primo comando ioctl ricevuto
		if(filp->private_data==NULL){
			filp->private_data= kzalloc(sizeof(session_ioctl_op),GFP_ATOMIC);
		}
		p=filp->private_data;
		time = (int *)param;
		p->jiffies_recv=*time;
		p->set_recv_timeout= true;


		return 0;
	}

	else if(command==REVOKE_DELAYED_MEX){

		point=delayed_writes[my_minor];
		//si itera per ogni delayed_work_write registrato
		while((point!=NULL)&&(point->del_work!=NULL)){

			cancel_delayed_work_sync(point->del_work);
			point=point->next_delayed_work;
		}
		return 0;
	}

	//do here whathever you would like to cotrol the state of the device
	return 0;

}

static int dev_flush(struct file *filp,fl_owner_t id){
	int my_minor;
	delayed_write_node *point;

	my_minor = get_minor(filp);

	//printk("flush invoked\n");
	//viene settata una variabile globale su quel minor per risvegliare le read pendenti
	flushed[my_minor]=true;
	wake_up(&wait_queue);
	point=delayed_writes[my_minor];
	//si itera per ogni delayed_work_write registrato
	while(point!=NULL &&(point->del_work!=NULL)){

		cancel_delayed_work_sync(point->del_work);
		point=point->next_delayed_work;
	}
	return 0;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.write = dev_write,
	.read = dev_read,
	.open =  dev_open,
	.release = dev_release,
	.unlocked_ioctl = dev_ioctl,
	.flush = dev_flush
};

//--------------------------------------------------------//

//MAX_MESSAGE_SIZE

// MAX_STORAGE_SIZE


int init_module(void) {

	int i; 
	//unica waitqueue per tutte le deferred read di questo driver
	init_waitqueue_head(&wait_queue);

	//initialize the drive internal state
	for(i=0;i<MINORS;i++){

		works[i] = kzalloc(sizeof(message_node),GFP_ATOMIC);
		works[i]->buffer = NULL;
		works[i]->minor = i; 



		if (works[i] == NULL) {
			printk("%s: work_queue entry memory allocation failed\n",MODNAME);
			return -1;
		}

		delayed_writes[i]=kzalloc(sizeof(delayed_write_node),GFP_ATOMIC);


		//allocazione workqueue per le write
		wq_Write[i]=alloc_ordered_workqueue("%dw", WQ_MEM_RECLAIM,i);

		if(wq_Write[i]== NULL){
			printk("%s: failure to create workqueue %d\n",MODNAME,i);
			return -1;
		}
		printk("%s: work_queues %d allocation success - address is %p\n",MODNAME,i,works[i]);

	}

	Major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);


	if (Major < 0) {
		printk("%s: registering device failed\n",MODNAME);
		return Major;
	}

	printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n",MODNAME, Major);


	return 0;

}

void cleanup_module(void) {

	int i;
	for(i=0;i<MINORS;i++){
		kfree((void*)works[i]);
	}

	unregister_chrdev(Major, DEVICE_NAME);



	printk(KERN_INFO "%s: device unregistered, it was assigned major number %d\n",MODNAME, Major);



	return;

}
