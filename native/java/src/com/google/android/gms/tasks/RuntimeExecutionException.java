package com.google.android.gms.tasks;

public class RuntimeExecutionException extends RuntimeException {
    public RuntimeExecutionException(Throwable cause) { super(cause); }
    public RuntimeExecutionException(String message) { super(message); }
    public RuntimeExecutionException(String message, Throwable cause) { super(message, cause); }
}
