/*
 * EMERGENCY PATIENT PRIORITY SYSTEM
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define MAX_PATIENTS   200
#define MAX_REASONS    16
#define HISTORY_FILE   "history.txt"
#define DATA_FILE      "patients.dat"
#define REPORT_FILE    "report.txt"

// enums/constants  //
enum { CAT_RED, CAT_ORANGE, CAT_YELLOW, CAT_GREEN };

enum { CC_CHEST_PAIN = 1, CC_BREATHING, CC_BLEEDING, CC_FRACTURE,
       CC_FEVER, CC_BURN, CC_HEAD, CC_OTHER };

static const char *CATEGORY_NAME[] = { "RED", "ORANGE", "YELLOW", "GREEN" };
static const char *CATEGORY_DESC[] = { "Immediate Treatment", "Very Urgent",
                                       "Urgent", "Stable" };
static const char *COMPLAINT_NAME[] = { "", "Chest Pain", "Breathing Difficulty",
                                        "Excessive Bleeding", "Fracture",
                                        "High Fever", "Burn Injury",
                                        "Head Injury", "Other" };
/* Bonus points per chief complaint (index = complaint code).
   Chest 5, Breathing 5, Head 6, Bleeding 7 come from the design notes;
   Fracture/Fever/Burn/Other are added for completeness. */
static const int COMPLAINT_POINTS[] = { 0, 5, 5, 7, 3, 2, 4, 6, 0 };

static const char *CONSCIOUS_NAME[] = { "Unconscious", "Drowsy", "Alert" };

enum { ST_WAITING, ST_CALLED, ST_SERVED };
static const char *STATUS_NAME[] = { "Waiting", "Called", "Served" };

/* ---------- data structures ---------- */
typedef struct {
    char text[48];
    int  points;
} Reason;

typedef struct {
    int   id;
    char  name[50];
    int   age;
    char  gender;              
    int   heartRate;
    int   oxygen;
    int   systolicBP;
    int   diastolicBP;
    float temperature;         
    int   respiratoryRate;
    int   consciousness;       /* 0 Unconscious, 1 Drowsy, 2 Alert */
    int   painLevel;           /* 0-10 */
    int   injurySeverity;      /* 0-5 */
    int   complaint;           /* 1-8 */
    int   criticalScore;
    int   category;            /* CAT_* */
    int   emergency;           /* 1 = emergency alert triggered */
    int   status;              /* ST_* */
    int   arrival;             /* arrival order, used as tie-breaker */
    int   reasonCount;
    Reason reasons[MAX_REASONS];
} Patient;

static Patient patients[MAX_PATIENTS];
static int     patientCount = 0;
static int     nextArrival  = 1;
static int     analysisDone = 0;      /* 0 if data changed since last analysis */
static int     queue[MAX_PATIENTS];   /* indices into patients[], sorted */
static int     queueLen = 0;

/* ---------- input helpers ---------- */
static void readLine(char *buf, size_t n)
{
    if (!fgets(buf, (int)n, stdin)) {      /* EOF: exit cleanly */
        printf("\nInput closed. Exiting.\n");
        exit(0);
    }
    buf[strcspn(buf, "\r\n")] = '\0';
}

static int readInt(const char *prompt, int lo, int hi)
{
    char buf[64], *end;
    long v;
    for (;;) {
        printf("%s", prompt);
        readLine(buf, sizeof buf);
        v = strtol(buf, &end, 10);
        if (end != buf && *end == '\0' && v >= lo && v <= hi)
            return (int)v;
        printf("  Invalid input. Enter a whole number between %d and %d.\n", lo, hi);
    }
}

static float readFloat(const char *prompt, float lo, float hi)
{
    char buf[64], *end;
    float v;
    for (;;) {
        printf("%s", prompt);
        readLine(buf, sizeof buf);
        v = strtof(buf, &end);
        if (end != buf && *end == '\0' && v >= lo && v <= hi)
            return v;
        printf("  Invalid input. Enter a number between %.0f and %.0f.\n", lo, hi);
    }
}

static void readName(const char *prompt, char *out, size_t n)
{
    char buf[128];
    for (;;) {
        printf("%s", prompt);
        readLine(buf, sizeof buf);
        if (buf[0] != '\0') {
            strncpy(out, buf, n - 1);
            out[n - 1] = '\0';
            return;
        }
        printf("  Name cannot be empty.\n");
    }
}

static char readGender(void)
{
    char buf[16];
    for (;;) {
        printf("Gender (M/F/O): ");
        readLine(buf, sizeof buf);
        char c = (char)toupper((unsigned char)buf[0]);
        if (buf[1] == '\0' && (c == 'M' || c == 'F' || c == 'O'))
            return c;
        printf("  Enter M, F or O.\n");
    }
}

/* ---------- history log ---------- */
static void logEvent(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void logEvent(const char *fmt, ...)
{
    FILE *fp = fopen(HISTORY_FILE, "a");
    if (!fp) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(fp, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* ---------- lookup ---------- */
static int findById(int id)
{
    for (int i = 0; i < patientCount; i++)
        if (patients[i].id == id) return i;
    return -1;
}

/* ---------- scoring engine ---------- */
static void addReason(Patient *p, int points, const char *fmt, ...)
{
    if (points <= 0 || p->reasonCount >= MAX_REASONS) return;
    Reason *r = &p->reasons[p->reasonCount++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text, sizeof r->text, fmt, ap);
    va_end(ap);
    r->points = points;
    p->criticalScore += points;
}

static int isEmergency(const Patient *p)
{
    return p->oxygen < 80 || p->heartRate > 160 ||
           p->consciousness == 0 || p->systolicBP < 70;
}

static int categoryFromScore(int score)
{
    if (score >= 20) return CAT_RED;
    if (score >= 14) return CAT_ORANGE;
    if (score >= 8)  return CAT_YELLOW;
    return CAT_GREEN;
}

/* Compare vitals against normal ranges and accumulate risk points. */
static void evaluatePatient(Patient *p)
{
    p->criticalScore = 0;
    p->reasonCount   = 0;

    /* Heart rate (normal 60-100) */
    if (p->heartRate > 140)      addReason(p, 4, "Heart Rate %d bpm (>140)", p->heartRate);
    else if (p->heartRate > 120) addReason(p, 2, "Heart Rate %d bpm (>120)", p->heartRate);
    if (p->heartRate < 40)       addReason(p, 4, "Heart Rate %d bpm (<40)", p->heartRate);

    /* Oxygen saturation (normal 95-100) */
    if (p->oxygen < 85)          addReason(p, 6, "Oxygen Saturation %d%% (<85)", p->oxygen);
    else if (p->oxygen < 90)     addReason(p, 4, "Oxygen Saturation %d%% (<90)", p->oxygen);
    else if (p->oxygen < 95)     addReason(p, 2, "Oxygen Saturation %d%% (<95)", p->oxygen);

    /* Systolic BP (normal 90-120) */
    if (p->systolicBP < 80)      addReason(p, 4, "Systolic BP %d (<80)", p->systolicBP);
    else if (p->systolicBP < 90) addReason(p, 2, "Systolic BP %d (<90)", p->systolicBP);
    if (p->systolicBP > 180)     addReason(p, 4, "Systolic BP %d (>180)", p->systolicBP);

    /* Diastolic BP (normal 60-80) */
    if (p->diastolicBP > 120)    addReason(p, 3, "Diastolic BP %d (>120)", p->diastolicBP);
    else if (p->diastolicBP < 50) addReason(p, 2, "Diastolic BP %d (<50)", p->diastolicBP);

    /* Temperature (normal 97-99 F) */
    if (p->temperature > 103.0f)      addReason(p, 3, "Temperature %.1fF (>103)", p->temperature);
    else if (p->temperature > 101.0f) addReason(p, 1, "Temperature %.1fF (>101)", p->temperature);
    else if (p->temperature < 95.0f)  addReason(p, 3, "Temperature %.1fF (<95)", p->temperature);

    /* Respiratory rate (normal 12-20) */
    if (p->respiratoryRate > 30 || p->respiratoryRate < 8)
        addReason(p, 3, "Respiratory Rate %d/min (severe)", p->respiratoryRate);
    else if (p->respiratoryRate > 24)
        addReason(p, 2, "Respiratory Rate %d/min (>24)", p->respiratoryRate);

    /* Consciousness (heavy weight) */
    if (p->consciousness == 0)      addReason(p, 8, "Unconscious");
    else if (p->consciousness == 1) addReason(p, 4, "Drowsy");

    /* Pain */
    addReason(p, p->painLevel / 3, "Pain Level %d/10", p->painLevel);

    /* Injury severity (0-5 scale, added directly) */
    if (p->injurySeverity == 5)
        addReason(p, 5, "Life Threatening Injury");
    else
        addReason(p, p->injurySeverity, "Injury Severity %d/5", p->injurySeverity);

    /* Chief complaint */
    addReason(p, COMPLAINT_POINTS[p->complaint], "Complaint: %s",
              COMPLAINT_NAME[p->complaint]);

    /* Category + emergency override */
    p->category  = categoryFromScore(p->criticalScore);
    p->emergency = isEmergency(p);
    if (p->emergency)
        p->category = CAT_RED;
}

/* ---------- display helpers ---------- */
static void printAlert(const Patient *p)
{
    printf("\n********************************\n");
    printf("   EMERGENCY ALERT GENERATED\n");
    printf("   Patient: %s (ID %d)\n", p->name, p->id);
    printf("   Immediate Medical Attention\n");
    printf("********************************\n");
    printf("   Priority = RED\n\n");
}

static void printLine(char c, int n)
{
    while (n-- > 0) putchar(c);
    putchar('\n');
}

static void printQueueHeader(void)
{
    printf("%-4s %-5s %-20s %-4s %-6s %-7s %-8s %-9s\n",
           "Rank", "ID", "Name", "Age", "Score", "Alert", "Priority", "Status");
    printLine('-', 72);
}

static void printQueueRow(int rank, const Patient *p)
{
    printf("%-4d %-5d %-20.20s %-4d %-6d %-7s %-8s %-9s\n",
           rank, p->id, p->name, p->age, p->criticalScore,
           p->emergency ? "YES" : "-", CATEGORY_NAME[p->category],
           STATUS_NAME[p->status]);
}

static void explainPatient(const Patient *p)
{
    printf("\nWhy is %s's priority %s?\n", p->name, CATEGORY_NAME[p->category]);
    printLine('-', 44);
    printf("Patient: %s (ID %d)\n\n", p->name, p->id);
    if (p->reasonCount == 0)
        printf("  No abnormal findings.\n");
    for (int i = 0; i < p->reasonCount; i++)
        printf("  %-36s +%d\n", p->reasons[i].text, p->reasons[i].points);
    printf("\nTotal Score = %d\n", p->criticalScore);
    if (p->emergency)
        printf("Emergency override: life-threatening vital sign / unconscious.\n");
    printf("Assigned Category: %s (%s)\n", CATEGORY_NAME[p->category],
           CATEGORY_DESC[p->category]);
}

static void printPatientDetails(const Patient *p)
{
    printf("\nID: %d | Name: %s | Age: %d | Gender: %c\n", p->id, p->name, p->age, p->gender);
    printf("HR: %d bpm | SpO2: %d%% | BP: %d/%d | Temp: %.1fF | RR: %d/min\n",
           p->heartRate, p->oxygen, p->systolicBP, p->diastolicBP,
           p->temperature, p->respiratoryRate);
    printf("Consciousness: %s | Pain: %d/10 | Injury: %d/5 | Complaint: %s\n",
           CONSCIOUS_NAME[p->consciousness], p->painLevel, p->injurySeverity,
           COMPLAINT_NAME[p->complaint]);
    printf("Score: %d | Priority: %s (%s) | Status: %s%s\n",
           p->criticalScore, CATEGORY_NAME[p->category],
           CATEGORY_DESC[p->category], STATUS_NAME[p->status],
           p->emergency ? " | EMERGENCY ALERT" : "");
}

/* ---------- persistence ---------- */
static void saveData(void)
{
    FILE *fp = fopen(DATA_FILE, "wb");
    if (!fp) { printf("Warning: could not save %s\n", DATA_FILE); return; }
    fwrite(&patientCount, sizeof patientCount, 1, fp);
    fwrite(&nextArrival, sizeof nextArrival, 1, fp);
    fwrite(patients, sizeof(Patient), (size_t)patientCount, fp);
    fclose(fp);
}

static void loadData(void)
{
    FILE *fp = fopen(DATA_FILE, "rb");
    if (!fp) return;
    int count = 0, arr = 1;
    if (fread(&count, sizeof count, 1, fp) == 1 &&
        fread(&arr, sizeof arr, 1, fp) == 1 &&
        count >= 0 && count <= MAX_PATIENTS &&
        fread(patients, sizeof(Patient), (size_t)count, fp) == (size_t)count) {
        patientCount = count;
        nextArrival  = arr;
        printf("Loaded %d saved patient(s) from %s.\n", patientCount, DATA_FILE);
    } else {
        patientCount = 0;
        printf("Saved data unreadable; starting fresh.\n");
    }
    fclose(fp);
}

/* ---------- queue ---------- */
static int cmpPatients(const void *a, const void *b)
{
    const Patient *x = &patients[*(const int *)a];
    const Patient *y = &patients[*(const int *)b];
    if (x->emergency != y->emergency) return y->emergency - x->emergency;
    if (x->criticalScore != y->criticalScore) return y->criticalScore - x->criticalScore;
    return x->arrival - y->arrival;          /* earlier arrival first */
}

static void buildQueue(void)
{
    queueLen = 0;
    for (int i = 0; i < patientCount; i++)
        if (patients[i].status != ST_SERVED)
            queue[queueLen++] = i;
    qsort(queue, (size_t)queueLen, sizeof(int), cmpPatients);
}

/* ---------- menu actions ---------- */
static void registerPatient(void)
{
    if (patientCount >= MAX_PATIENTS) {
        printf("Patient capacity (%d) reached.\n", MAX_PATIENTS);
        return;
    }
    Patient p;
    memset(&p, 0, sizeof p);

    printf("\n--- Register Patient ---\n");
    for (;;) {
        p.id = readInt("Patient ID: ", 1, 999999);
        if (findById(p.id) < 0) break;
        printf("  ID already exists. Choose another.\n");
    }
    readName("Patient Name: ", p.name, sizeof p.name);
    p.age    = readInt("Age: ", 0, 120);
    p.gender = readGender();

    printf("\n-- Vital Signs --\n");
    p.heartRate       = readInt("Heart Rate (BPM): ", 0, 300);
    p.oxygen          = readInt("Oxygen Saturation (%): ", 0, 100);
    p.systolicBP      = readInt("Systolic BP (Upper Value): ", 0, 300);
    p.diastolicBP     = readInt("Diastolic BP (Lower Value): ", 0, 200);
    p.temperature     = readFloat("Body Temperature (F): ", 80.0f, 115.0f);
    p.respiratoryRate = readInt("Respiratory Rate (breaths/min): ", 0, 80);

    printf("\nConsciousness Level:\n  0 - Unconscious\n  1 - Drowsy\n  2 - Alert\n");
    p.consciousness = readInt("Choice: ", 0, 2);

    printf("\nPain Level:\n  0 - No Pain | 1-3 Mild | 4-6 Moderate | 7-10 Severe\n");
    p.painLevel = readInt("Pain (0-10): ", 0, 10);

    printf("\nInjury Severity:\n  0 None | 1 Minor | 2 Moderate | 3 Serious | "
           "4 Severe | 5 Life Threatening\n");
    p.injurySeverity = readInt("Severity (0-5): ", 0, 5);

    printf("\nChief Complaint:\n");
    for (int i = 1; i <= 8; i++)
        printf("  %d. %s\n", i, COMPLAINT_NAME[i]);
    p.complaint = readInt("Choice: ", 1, 8);

    p.status  = ST_WAITING;
    p.arrival = nextArrival++;

    /* Preliminary score so the entry is usable immediately; the real
       ordering is decided in "Run Priority Analysis". */
    evaluatePatient(&p);
    patients[patientCount++] = p;
    analysisDone = 0;

    printf("\nPatient registered. Run Priority Analysis (option 2) to rank all patients.\n");
    logEvent("Patient Registered: ID %d, %s", p.id, p.name);
    saveData();
}

static void runPriorityAnalysis(void)
{
    int waiting = 0;
    for (int i = 0; i < patientCount; i++)
        if (patients[i].status != ST_SERVED) waiting++;
    if (waiting == 0) {
        printf("\nNo active patients to evaluate.\n");
        return;
    }

    printf("\n--- Running Priority Analysis ---\n");
    for (int i = 0; i < patientCount; i++) {
        Patient *p = &patients[i];
        if (p->status == ST_SERVED) continue;
        int wasEmergency = p->emergency;
        evaluatePatient(p);
        printf("Patient %s Score = %d\n", p->name, p->criticalScore);
        if (p->emergency) {
            printAlert(p);
            if (!wasEmergency || !analysisDone)
                logEvent("Emergency Alert Triggered: ID %d, %s", p->id, p->name);
        }
    }
    buildQueue();
    analysisDone = 1;
    printf("Priority queue created for %d patient(s).\n", queueLen);
    logEvent("Priority Analysis Completed: %d patient(s) ranked", queueLen);
    saveData();
}

static int ensureAnalysis(void)
{
    if (!analysisDone) {
        printf("\nData changed since last analysis - running Priority Analysis first.\n");
        runPriorityAnalysis();
    }
    return queueLen > 0;
}

static void viewQueue(void)
{
    if (!ensureAnalysis()) { printf("\nQueue is empty.\n"); return; }
    printf("\n========== PRIORITY QUEUE ==========\n");
    printQueueHeader();
    for (int i = 0; i < queueLen; i++)
        printQueueRow(i + 1, &patients[queue[i]]);
}

static void callNextPatient(void)
{
    if (!ensureAnalysis()) { printf("\nNo patients waiting.\n"); return; }
    for (int i = 0; i < queueLen; i++) {
        Patient *p = &patients[queue[i]];
        if (p->status == ST_WAITING) {
            p->status = ST_CALLED;
            printf("\n>>> CALLING NEXT PATIENT <<<\n");
            printPatientDetails(p);
            explainPatient(p);
            logEvent("Patient Called: ID %d, %s (%s, score %d)",
                     p->id, p->name, CATEGORY_NAME[p->category], p->criticalScore);
            saveData();
            return;
        }
    }
    printf("\nNo patients waiting (all remaining have already been called).\n");
}

static void searchPatient(void)
{
    printf("\n--- Search Patient ---\n1. By ID\n2. By Name\n");
    int ch = readInt("Choice: ", 1, 2);
    int found = 0;

    if (ch == 1) {
        int id = readInt("Enter ID: ", 1, 999999);
        int idx = findById(id);
        if (idx >= 0) {
            printPatientDetails(&patients[idx]);
            explainPatient(&patients[idx]);
            found = 1;
        }
    } else {
        char key[50], lowName[50], lowKey[50];
        readName("Enter name (or part of it): ", key, sizeof key);
        for (size_t i = 0; key[i]; i++) lowKey[i] = (char)tolower((unsigned char)key[i]);
        lowKey[strlen(key)] = '\0';
        for (int i = 0; i < patientCount; i++) {
            size_t k;
            for (k = 0; patients[i].name[k]; k++)
                lowName[k] = (char)tolower((unsigned char)patients[i].name[k]);
            lowName[k] = '\0';
            if (strstr(lowName, lowKey)) {
                printPatientDetails(&patients[i]);
                explainPatient(&patients[i]);
                found = 1;
            }
        }
    }
    if (!found) printf("No matching patient found.\n");
}

static void markServed(void)
{
    int id = readInt("\nEnter ID of patient to mark as served: ", 1, 999999);
    int idx = findById(id);
    if (idx < 0) { printf("Patient not found.\n"); return; }
    Patient *p = &patients[idx];
    if (p->status == ST_SERVED) { printf("Patient already served.\n"); return; }
    p->status = ST_SERVED;
    printf("Patient %s (ID %d) marked as served.\n", p->name, p->id);
    logEvent("Patient Served: ID %d, %s", p->id, p->name);
    buildQueue();                 /* keep queue in sync without rescoring */
    saveData();
}

static void generateReport(void)
{
    int cat[4] = {0}, served = 0, waiting = 0, maxScore = 0, alerts = 0;
    long sum = 0;
    for (int i = 0; i < patientCount; i++) {
        const Patient *p = &patients[i];
        cat[p->category]++;
        if (p->status == ST_SERVED) served++; else waiting++;
        if (p->criticalScore > maxScore) maxScore = p->criticalScore;
        if (p->emergency) alerts++;
        sum += p->criticalScore;
    }
    double avg = patientCount ? (double)sum / patientCount : 0.0;

    FILE *out[2] = { stdout, fopen(REPORT_FILE, "w") };
    for (int k = 0; k < 2; k++) {
        FILE *f = out[k];
        if (!f) continue;
        fprintf(f, "\n========== REPORT ==========\n\n");
        fprintf(f, "Total Patients  : %d\n\n", patientCount);
        fprintf(f, "RED Patients    : %d\n", cat[CAT_RED]);
        fprintf(f, "ORANGE Patients : %d\n", cat[CAT_ORANGE]);
        fprintf(f, "YELLOW Patients : %d\n", cat[CAT_YELLOW]);
        fprintf(f, "GREEN Patients  : %d\n\n", cat[CAT_GREEN]);
        fprintf(f, "Emergency Alerts: %d\n", alerts);
        fprintf(f, "Served Patients : %d\n", served);
        fprintf(f, "Waiting Patients: %d\n\n", waiting);
        fprintf(f, "Highest Score   : %d\n", maxScore);
        fprintf(f, "Average Score   : %.2f\n", avg);
        fprintf(f, "============================\n");
    }
    if (out[1]) {
        fclose(out[1]);
        printf("Report saved to %s\n", REPORT_FILE);
    }
    logEvent("Report Generated: %d patient(s)", patientCount);
}

static void menu(void)
{
    printf("\n====== EMERGENCY PATIENT PRIORITY SYSTEM ======\n");
    printf("1. Register Patient\n");
    printf("2. Run Priority Analysis\n");
    printf("3. View Priority Queue\n");
    printf("4. Call Next Patient\n");
    printf("5. Search Patient\n");
    printf("6. Mark Served\n");
    printf("7. Generate Report\n");
    printf("8. Exit\n");
}

int main(void)
{
    loadData();
    if (patientCount > 0) {
        for (int i = 0; i < patientCount; i++) evaluatePatient(&patients[i]);
        buildQueue();
        analysisDone = 1;
    }
    logEvent("System Started");

    for (;;) {
        menu();
        switch (readInt("Enter choice: ", 1, 8)) {
            case 1: registerPatient();     break;
            case 2: runPriorityAnalysis(); break;
            case 3: viewQueue();           break;
            case 4: callNextPatient();     break;
            case 5: searchPatient();       break;
            case 6: markServed();          break;
            case 7: generateReport();      break;
            case 8:
                saveData();
                logEvent("System Exited");
                printf("Data saved. Goodbye.\n");
                return 0;
        }
    }
}
