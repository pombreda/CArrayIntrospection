/**
 * This check tests we print info about both
 * array arguments when two array arguments are
 * given, and both have different sentinel checks
 *
 * We expect to find one non-optional sentinel check for string 
 * and 1 for string2.
 **/
void printc(char c) {}
int find(char string[], char string2[], int distance) {
	for (int i = 0; i < distance; i++) {
		if (string[i] == '\0') {
			break;
		}
		if (string2[i] == '\0')
			break;
	}
	return 1;
}
