#include "types.h"
#include "user.h"

int main(void)
{
     int pid1, pid2, pid3;
     int count = 10; // 각 프로세스가 출력할 횟수

     ps(0);

     pid1 = fork();
     if (pid1 == 0) // child
     {
          setnice(getpid(), 15);

          for (int i = 0; i < count; i++)
          {
               printf(1, "First Child - Process %d\n", getpid());
               ps(0);
          }
          exit();
     }
     else // parent
     {
          pid2 = fork();
          if (pid2 == 0)
          {
               setnice(getpid(), 5);

               for (int i = 0; i < count; i++)
               {
                    printf(1, "Second Child - Process %d\n", getpid());
                    ps(0);
               }
               exit();
          }
          else
          {
               pid3 = fork();
               if (pid3 == 0)
               {
                    setnice(getpid(), 0);

                    for (int i = 0; i < count; i++)
                    {
                         printf(1, "Third Child - Process %d\n", getpid());
                         ps(0);
                    }
                    exit();
               }
               else
               {
                    // Parent waits for all children to finish
                    wait();
                    wait();
                    wait();
               }
          }
     }
     exit();
}