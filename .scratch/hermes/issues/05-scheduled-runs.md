# 05: Scheduled Runs

Status: in-review

**What to build:** From the drawer, the jobs that have delivered something
(`SCHEDULED_JOBS_REQUEST`), each with its schedule, age and unread count;
a job opens its newest run (`SCHEDULED_RUN_REQUEST`, offset 0) and steps
to older ones while `older` is true. Three states, as the bridge
distinguishes them: no runs, the store unreadable, and a bridge without
the page.

**Blocked by:** 02

## Comments

In the simulator: the jobs are asked for once, a job opens its newest run,
and Older asks for offset 1. A run's parts are drawn as the transcript's
messages are.
