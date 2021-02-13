#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define THREADCOUNT 6
#define COPYBUFFER 5120
// ==============================
pthread_t cpthr[THREADCOUNT];
pthread_t crawlthr;
int working_thr;
int crawl_done;
int cp_thr_flag;
// ==============================
pthread_barrier_t crawl_output, thread_sync;
pthread_mutex_t main_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cp_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cp_resume = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_resume = PTHREAD_COND_INITIALIZER;
// ==============================
struct crawler_args {
   char *path, *dest;
   struct crawler_args* retval;
   int pathsize;
};

struct task {
   char *from, *dest;
   struct task* next;
};

struct task_queue {
   struct task* head;
   struct task* tail;
   int terminate;
};
// main task queue
struct task_queue* tk_queue;
// ==============================
void mmalloc(void* p, int size);
int return_size(char* str);
void path_init(char** pstr, char* str);
void crawler_args_init(struct crawler_args** args, char* from, char* dest);
void task_queue_init(struct task_queue** queue);
// ----------------------------------------------------------------------
void task_add(struct task_queue** queue, char* from, char* dest);
void task_remove(struct task_queue** queue);
void task_get(struct task_queue* queue, struct task** p);
// ----------------------------------------------------------------------
void copy_file(char* from, char* dest);
void copy_once(char* from, char* dest);
int dircrawler(char* path, char* dest, int pathsize, struct crawler_args* args);
// ----------------------------------------------------------------------
void* crawl_thr(void* args);
void* cp_thr();
// ==============================
int main(int argc, char* argv[])
{
   char *from, *dest;
   path_init(&from, argv[1]);
   path_init(&dest, argv[2]);
   struct stat s;
   if (stat(from, &s) == 0) {
      if (s.st_mode & S_IFDIR) {
         struct crawler_args* cargs;
         crawl_done = 0;
         working_thr = 0;

         pthread_barrier_init(&crawl_output, NULL, 2);
         pthread_barrier_init(&thread_sync, NULL, THREADCOUNT + 1);

         crawler_args_init(&cargs, from, dest);
         task_queue_init(&tk_queue);

         pthread_create(&crawlthr, NULL, crawl_thr, (void*)cargs);
         for (int i = 0; i < THREADCOUNT; ++i) {
            pthread_create(&(cpthr[i]), NULL, cp_thr, NULL);
            pthread_detach(cpthr[i]);
         }

         pthread_barrier_wait(&thread_sync);

         while (1) {
            pthread_barrier_wait(&crawl_output);

            if (crawl_done)
               break;

            task_add(&tk_queue, cargs->retval->path, cargs->retval->dest);

            pthread_barrier_wait(&crawl_output);
         };
         while (working_thr)
            ;
         tk_queue->terminate = 1;
         pthread_cond_broadcast(&cp_resume);
         for (int i = 0; i < working_thr; ++i)
            pthread_join(cpthr[i], NULL);

         free(cargs->retval);
         free(cargs);
      } else {
         copy_once(from, dest);
      }
   }
   free(from);
   free(dest);

   return 0;
}
// ==============================
void mmalloc(void* p, int size)
{

   void** pp = p;
   void* temp = malloc(size);
   if (!temp) {
      fprintf(stdout, "Error allocating memory\n");
      exit(EXIT_FAILURE);
   }

   *pp = temp;
   return;
}

int return_size(char* str)
{
   int size = 0;
   char* temp = str;
   while (*temp) {
      ++size;
      ++temp;
   }
   return size;
}

void path_init(char** pstr, char* str)
{
   int size = return_size(str);
   mmalloc(pstr, size + 1);
   strcpy(*pstr, str);
   if ((*pstr)[size - 1] == '/')
      (*pstr)[size - 1] = '\0';
   return;
}

void crawler_args_init(struct crawler_args** args, char* from, char* dest)
{
   mmalloc(args, sizeof(struct crawler_args));
   mmalloc(&((*args)->retval), sizeof(struct crawler_args));
   (*args)->path = strdup(from);
   (*args)->dest = strdup(dest);
   (*args)->pathsize = return_size(from);
   return;
}
// ==============================
void task_queue_init(struct task_queue** queue)
{
   mmalloc(queue, sizeof(struct task_queue));
   (*queue)->head = NULL;
   (*queue)->tail = NULL;
   (*queue)->terminate = 0;
   return;
}

void task_add(struct task_queue** queue, char* from, char* dest)
{
   pthread_mutex_lock(&queue_mtx);
   struct task* newtask;
   mmalloc(&newtask, sizeof(struct task));

   newtask->from = strdup(from);
   newtask->dest = strdup(dest);
   newtask->next = NULL;

   if ((*queue)->head == NULL) {
      (*queue)->tail = newtask;
      (*queue)->head = newtask;
   } else {
      (*queue)->tail->next = newtask;
      (*queue)->tail = newtask;
   }

   while (working_thr == THREADCOUNT) {
      pthread_cond_wait(&queue_resume, &queue_mtx);
   }

   pthread_cond_signal(&cp_resume);
   pthread_mutex_unlock(&queue_mtx);
   return;
}

void task_remove(struct task_queue** queue)
{
   pthread_mutex_lock(&queue_mtx);
   struct task* old_task = (*queue)->head;
   (*queue)->head = (*queue)->head->next;
   free(old_task->from);
   free(old_task->dest);
   free(old_task);
   pthread_mutex_unlock(&queue_mtx);
   return;
}

void task_get(struct task_queue* queue, struct task** p)
{
   pthread_mutex_lock(&queue_mtx);
   if (queue->head != NULL && queue->head != NULL) {
      mmalloc(p, sizeof(struct task));
      (*p)->from = strdup(queue->head->from);
      (*p)->dest = strdup(queue->head->dest);
   }

   pthread_mutex_unlock(&queue_mtx);
   return;
}
// ==============================
void copy_file(char* from, char* dest)
{
   printf("COPYING: %s %s\n", from, dest);
   FILE* fpfrom = fopen(from, "r");
   FILE* fpdest = fopen(dest, "w");
   char buf[COPYBUFFER];
   int bufsize;
   while (!feof(fpfrom)) {
      if ((bufsize = fread(buf, 1, sizeof(buf), fpfrom)))
         fwrite(buf, 1, bufsize, fpdest);
   }
   fclose(fpfrom);
   fclose(fpdest);
   return;
}

void copy_once(char* from, char* dest)
{
   int index;
   for (index = return_size(from) - 1; from[index] != '/'; --index)
      ;
   index++;
   char* buf;
   int bufsize = snprintf(NULL, 0, "%s/%s", dest, from + index) + 1;
   mmalloc(&buf, bufsize);
   snprintf(buf, bufsize, "%s/%s", dest, from + index);
   copy_file(from, buf);
   return;
}
// ==============================
int dircrawler(char* path, char* dest, int pathsize, struct crawler_args* args)
{
   // dirent variables and structures
   DIR* dp;
   struct dirent* entry;
   // buffers
   size_t bufsize;
   char *buf1, *buf2;
   buf1 = buf2 = NULL;

   if ((dp = opendir(path)) != NULL) {
      while ((entry = readdir(dp)) != NULL) {
         // skip over . and ..
         if (!(strcmp(entry->d_name, ".")) || !(strcmp(entry->d_name, "..")))
            continue;

         if (buf1 != NULL)
            free(buf1);
         if (buf2 != NULL)
            free(buf2);

         // buf1: absolute new path, buf2: relative destination path
         bufsize = snprintf(NULL, 0, "%s/%s", path, entry->d_name) + 1;
         mmalloc(&buf1, bufsize); // buf1
         snprintf(buf1, bufsize, "%s/%s", path, entry->d_name);

         bufsize = snprintf(NULL, 0, "%s%s/%s", dest, (path + pathsize - 1), entry->d_name) + 1;
         mmalloc(&buf2, bufsize); // buf2
         snprintf(buf2, bufsize, "%s%s/%s", dest, (path + pathsize), entry->d_name);

         if (entry->d_type == DT_DIR) {
            // make new directory in dest
            mkdir(buf2, 0755);
            if (dircrawler(buf1, dest, pathsize, args))
               closedir(dp);

         } else {
            args->path = strdup(buf1);
            args->dest = strdup(buf2);
            pthread_barrier_wait(&crawl_output);
            pthread_barrier_wait(&crawl_output);
            free(args->path);
            free(args->dest);
         }
      }
   }
   if (buf1 != NULL)
      free(buf1);
   if (buf2 != NULL)
      free(buf2);

   closedir(dp);
   return 0;
}
// ==============================
void* crawl_thr(void* args)
{
   struct crawler_args* temp = (struct crawler_args*)args;
   dircrawler(temp->path, temp->dest, temp->pathsize, temp->retval);
   // termination
   crawl_done = 1;
   pthread_barrier_wait(&crawl_output);
   free(temp->path);
   free(temp->dest);
   return (void*)0;
}

void* cp_thr()
{
   struct task* temp;
   pthread_barrier_wait(&thread_sync);
   while (1) {
      pthread_mutex_lock(&cp_mtx);
      while (tk_queue->head == NULL && !tk_queue->terminate) {
         pthread_cond_wait(&cp_resume, &cp_mtx);
      }

      if (tk_queue->terminate)
         break;

      task_get(tk_queue, &temp);
      task_remove(&tk_queue);
      ++working_thr;
      pthread_mutex_unlock(&cp_mtx);

      if (temp != NULL) {
         copy_file(temp->from, temp->dest);
         free(temp->dest);
         free(temp->from);
         free(temp);
      }

      pthread_mutex_lock(&cp_mtx);
      --working_thr;
      pthread_cond_signal(&queue_resume);
      pthread_mutex_unlock(&cp_mtx);
   }
   return (void*)0;
}
// ==============================

