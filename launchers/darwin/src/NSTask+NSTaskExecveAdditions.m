#import "NSTask+NSTaskExecveAdditions.h"

#import <libgen.h>

char **
toCArray(NSArray<NSString *> *array)
{
    char **cArray = (char **) malloc(sizeof(char *) * [array count]);
    if (cArray == NULL) {
        NSException *exception = [NSException
                                  exceptionWithName:@"MemoryException"
                                  reason:@"malloc failed"
                                  userInfo:nil];
        @throw exception;
    }
    for (int i = 0; i < [array count]; i++) {
        asprintf(&cArray[i], "%s", [array[i] UTF8String]);
    }
    return cArray;
}

@implementation NSTask (NSTaskExecveAdditions)

- (void) replaceThisProcess {
    char *path;
    asprintf(&path, "%s", [[self launchPath] UTF8String]);
    char **args = toCArray([self arguments]);

    NSMutableArray *env = [[NSMutableArray alloc] init];
    NSDictionary* environvment = [[NSProcessInfo processInfo] environment];
    for (NSString* key in environvment) {
        NSString* environmentVariable = [[key stringByAppendingString:@"="] stringByAppendingString:environvment[key]];
        [env addObject:environmentVariable];
    }

    char** envp = toCArray(env);
    // `execve` replaces the current process with `path`.
    // It will only return if it fails to replace the current process.
    chdir(dirname(path));
    execve(path, (char * const *)args, envp);

    // If we're here `execve` failed. :(
    for (int i = 0; i < [[self arguments] count]; i++) {
        free((void *) args[i]);
    }
    free((void *) args);
    free((void *) path);

    NSException *exception = [NSException
                              exceptionWithName:@"ExecveException"
                              reason:[NSString stringWithFormat:@"couldn't execve: %s", strerror(errno)]
                              userInfo:nil];
   @throw exception;
}

@end
