package android.view;

public class InflateException extends RuntimeException {
    public InflateException() {}
    public InflateException(String message, Throwable cause) { super(message, cause); }
    public InflateException(String message) { super(message); }
    public InflateException(Throwable cause) { super(cause); }
}
