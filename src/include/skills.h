#ifndef SKILLS_H
#define SKILLS_H

#include <stdbool.h>
#include "common.h"

typedef struct SkillsLoader SkillsLoader;

SkillsLoader* skills_loader_create(const char* workspace);
void skills_loader_destroy(SkillsLoader* loader);

StringArray* skills_loader_list_skills(SkillsLoader* loader, bool filter_unavailable);
char* skills_loader_load_skill(SkillsLoader* loader, const char* name);
char* skills_loader_load_skills_for_context(SkillsLoader* loader, StringArray* skill_names);
char* skills_loader_build_skills_summary(SkillsLoader* loader);
StringArray* skills_loader_get_always_skills(SkillsLoader* loader);

#endif // SKILLS_H