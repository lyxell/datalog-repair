import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public class Test {

    Map<Class<?>, String> concurrentMap = new ConcurrentHashMap<>();
    Map<String, Integer> myMap;

    Integer test1(String key) {
        Integer result = myMap.get(key);
        if (result == null) {
            result = key.length() + 10;
            myMap.put(key, result);
        }
        return result;
    }

    public String test2(Class<?> x, String name) {
        String str = concurrentMap.get(x);
        if (str == null) {
            str = "{" + name + ":" + x.getName() + "}";
            concurrentMap.put(x, str);
        }
        return str;
    }

    public static void main(String[] args) {
        Test test = new Test();
        System.out.println(test.test1("hello"));
    }
}
