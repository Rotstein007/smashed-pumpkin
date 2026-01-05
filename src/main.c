#include "app.h"
#include "smashed-pumpkin-resources.h"

int
main(int argc, char *argv[])
{
  smashed_pumpkin_register_resource();
  return g_application_run(G_APPLICATION(pumpkin_app_new()), argc, argv);
}
